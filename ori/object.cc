/*
 * Copyright (c) 2012 Stanford University
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR(S) DISCLAIM ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL AUTHORS BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

/*
 * TODO:
 *  - Object Compression
 */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>

#include <unistd.h>
#include <sys/param.h>
#include <sys/types.h>
#include <sys/stat.h>

#include <iostream>
#include <iomanip>
#include <sstream>
#include <string>

#include <openssl/sha.h>

#ifdef OPENSSL_NO_SHA256
#error "SHA256 not supported!"
#endif

#include "object.h"

using namespace std;

Object::Object()
{
    fd = -1;
}

Object::~Object()
{
    close();
}

/*
 * Create a new object.
 */
int
Object::create(const string &path, Type type)
{
    int status;

    fd = ::open(path.c_str(), O_CREAT | O_RDWR,
	        S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
    if (fd < 0)
	return -errno;

    switch (type) {
	case Commit:
	    status = write(fd, "CMMT", 4);
	    t = Commit;
	    break;
	case Tree:
	    status = write(fd, "TREE", 4);
	    t = Tree;
	    break;
	case Blob:
	    status = write(fd, "BLOB", 4);
	    t = Blob;
	    break;
	default:
	    printf("Unknown object type!\n");
	    assert(false);
	    break;
    }
    if (status < 0)
	return -errno;

    assert(status == 4);

    return 0;
}

/*
 * Open an existing object read-only.
 */
int
Object::open(const string &path)
{
    int status;
    char buf[5];

    fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0)
	return -errno;

    buf[4] = '\0';
    status = read(fd, buf, 4);
    if (status < 0) {
	::close(fd);
	fd = -1;
	return -errno;
    }

    assert(status == 4);

    if (strcmp(buf, "CMMT") == 0) {
	t = Commit;
    } else if (strcmp(buf, "TREE") == 0) {
	t = Tree;
    } else if (strcmp(buf, "BLOB") == 0) {
	t = Blob;
    } else {
	printf("Unknown object type!\n");
	assert(false);
    }

    return 0;
}

/*
 * Close the object file.
 */
void
Object::close()
{
    if (fd != -1)
        ::close(fd);
    fd = -1;
}

/*
 * Read the object type.
 */
Object::Type
Object::getType()
{
    return t;
}

/*
 * Get the on-disk file size including the object header.
 */
size_t
Object::getDiskSize()
{
    struct stat sb;

    if (fstat(fd, &sb) < 0) {
	return -errno;
    }

    return sb.st_size;
}

/*
 * Get the extracted object size.
 */
size_t
Object::getObjectSize()
{
    struct stat sb;

    if (fstat(fd, &sb) < 0) {
	return -errno;
    }

    return sb.st_size - ORI_OBJECT_HDRSIZE;
}

#define COPYFILE_BUFSZ	4096

/*
 * Append the specified file into the object.
 */
int
Object::appendFile(const string &path)
{
    int srcFd;
    char buf[COPYFILE_BUFSZ];
    struct stat sb;
    size_t bytesLeft;
    size_t bytesRead, bytesWritten;

    srcFd = ::open(path.c_str(), O_RDONLY);
    if (srcFd < 0)
	return -errno;

    if (fstat(srcFd, &sb) < 0) {
	::close(srcFd);
	return -errno;
    }

    bytesLeft = sb.st_size;
    while(bytesLeft > 0) {
	bytesRead = read(srcFd, buf, MIN(bytesLeft, COPYFILE_BUFSZ));
	if (bytesRead < 0) {
	    if (errno == EINTR)
		continue;
	    goto error;
	}

retryWrite:
	bytesWritten = write(fd, buf, bytesRead);
	if (bytesWritten < 0) {
	    if (errno == EINTR)
		goto retryWrite;
	    goto error;
	}

	// XXX: Need to handle this case!
	assert(bytesRead == bytesWritten);

	bytesLeft -= bytesRead;
    }

    ::close(srcFd);
    return sb.st_size;

error:
    ::close(srcFd);
    return -errno;
}

/*
 * Extract the contents of the object file into the specified path.
 */
int
Object::extractFile(const string &path)
{
    int dstFd;
    char buf[COPYFILE_BUFSZ];
    struct stat sb;
    size_t bytesLeft;
    size_t bytesRead, bytesWritten;

    dstFd = ::open(path.c_str(), O_WRONLY | O_CREAT,
		   S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
    if (dstFd < 0) {
	return -errno;
    }

    if (fstat(fd, &sb) < 0) {
	unlink(path.c_str());
	::close(dstFd);
	return -errno;
    }

    bytesLeft = sb.st_size - ORI_OBJECT_HDRSIZE;
    while(bytesLeft > 0) {
	bytesRead = read(fd, buf, MIN(bytesLeft, COPYFILE_BUFSZ));
	if (bytesRead < 0) {
	    if (errno == EINTR)
		continue;
	    goto error;
	}

retryWrite:
	bytesWritten = write(dstFd, buf, bytesRead);
	if (bytesWritten < 0) {
	    if (errno == EINTR)
		goto retryWrite;
	    goto error;
	}

	// XXX: Need to handle this case!
	assert(bytesRead == bytesWritten);

	bytesLeft -= bytesRead;
    }

    ::close(dstFd);
    return sb.st_size;

error:
    unlink(path.c_str());
    ::close(dstFd);
    return -errno;
}

/*
 * Append a blob to the object file.
 */
int
Object::appendBlob(const string &blob)
{
    int status;

    status = write(fd, blob.data(), blob.length());
    if (status < 0)
	return -errno;

    assert(status == blob.length());

    return 0;
}

/*
 * Extract the blob from the object file.
 */
string
Object::extractBlob()
{
    int status;
    size_t length = getObjectSize();
    char *buf;
    string rval;

    assert(length >= 0);

    buf = new char[length];
    status = read(fd, buf, length);
    if (status < 0)
	return "";

    assert(status == length);

    rval.assign(buf, length);

    return rval;
}

/*
 * Recompute the SHA-256 hash to verify the file.
 */
string
Object::computeHash()
{
    char buf[COPYFILE_BUFSZ];
    size_t bytesLeft;
    size_t bytesRead;
    SHA256_CTX state;
    unsigned char hash[SHA256_DIGEST_LENGTH];
    stringstream rval;

    SHA256_Init(&state);

    bytesLeft = getObjectSize();
    while(bytesLeft > 0) {
	bytesRead = read(fd, buf, MIN(bytesLeft, COPYFILE_BUFSZ));
	if (bytesRead < 0) {
	    if (errno == EINTR)
		continue;
	    return "";
	}

	SHA256_Update(&state, buf, bytesRead);
	bytesLeft -= bytesRead;
    }

    SHA256_Final(hash, &state);

    // Convert into string.
    for (int i = 0; i < SHA256_DIGEST_LENGTH; i++)
    {
	rval << hex << setw(2) << setfill('0') << (int)hash[i];
    }

    return rval.str();
}

