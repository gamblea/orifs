#include <cassert>

#include <sys/stat.h>
#include <sys/param.h>
#include <sys/types.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>

#include "debug.h"
#include "stream.h"



#define COPYFILE_BUFSZ 2048

std::string bytestream::readAll() {
    std::string rval;

    if (sizeHint() == 0) {
        // Need to read to end
        uint8_t buf[COPYFILE_BUFSZ];
        while (!ended()) {
            size_t n = read(buf, COPYFILE_BUFSZ);
            if (error()) {
                return "";
            }

            size_t oldSize = rval.size();
            rval.resize(oldSize + n);
            memcpy(&rval[oldSize], buf, n);
        }
        return rval;
    }
    else {
        rval.resize(sizeHint());
        read((uint8_t*)&rval[0], sizeHint());
        if (error()) {
            return "";
        }
        return rval;
    }
}

int bytestream::copyToFile(const std::string &path) {
    int dstFd = ::open(path.c_str(), O_WRONLY | O_CREAT,
            S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
    if (dstFd < 0)
        return -errno;

    int rval = copyToFd(dstFd);
    if (rval < 0) {
        ::close(dstFd);
        unlink(path.c_str());
    }
    return rval;
}

int bytestream::copyToFd(int dstFd)
{
    size_t totalWritten = 0;
    uint8_t buf[COPYFILE_BUFSZ];
    while (!ended()) {
        size_t bytesRead = read(buf, COPYFILE_BUFSZ);
        if (error()) return -errnum();
retryWrite:
        ssize_t bytesWritten = write(dstFd, buf, bytesRead);
        if (bytesWritten < 0) {
            if (errno == EINTR)
                goto retryWrite;
            return -errno;
        }
        totalWritten += bytesWritten;
    }

    return totalWritten;
}


void bytestream::setErrno(const char *msg) {
    char buf[512];
    snprintf(buf, 512, "%s: %s (%d)\n", msg, strerror(errno), errno);
    last_error.assign(buf);
    last_errnum = errno;
}

bool bytestream::inheritError(bytestream *bs) {
    if (bs->error()) {
        last_error.assign(bs->error());
        last_errnum = bs->errnum();
        return true;
    }
    return false;
}

/*
 * fdstream
 */

// TODO: error checking

fdstream::fdstream(int fd, off_t offset, size_t length)
    : fd(fd), offset(offset), length(length), left(length)
{
    if (lseek(fd, offset, SEEK_SET) != offset) {
        setErrno("lseek");
    }
}

bool fdstream::ended() {
    return left == 0 || error();
}

size_t fdstream::read(uint8_t *buf, size_t n) {
    size_t final_size = MIN(n, left);
retry_read:
    ssize_t read_bytes = ::read(fd, buf, final_size);
    if (read_bytes < 0) {
        if (errno == EINTR)
            goto retry_read;
        setErrno("read");
        return 0;
    }
    left -= read_bytes;
    return read_bytes;
}

size_t fdstream::sizeHint() const {
    return length;
}

/*
 * diskstream
 */

diskstream::diskstream(const std::string &filename)
    : fd(-1)
{
    fd = open(filename.c_str(), O_RDONLY);
    if (fd < 0)
        setErrno("open");

    size_t length = lseek(fd, 0, SEEK_END);
    source.reset(new fdstream(fd, 0, length));
}

diskstream::~diskstream() {
    if (fd > 0)
        close(fd);
}

bool diskstream::ended() {
    return source->ended();
}

size_t diskstream::read(uint8_t *buf, size_t n) {
    return source->read(buf, n);
}

size_t diskstream::sizeHint() const {
    return source->sizeHint();
}

/*
 * lzmastream
 */

lzmastream::lzmastream(bytestream *source, size_t size_hint)
    : source(source), size_hint(size_hint), output_ended(false)
{
    assert(source != NULL);

    lzma_stream strm2 = LZMA_STREAM_INIT;
    memcpy(&strm, &strm2, sizeof(lzma_stream));

    lzma_ret ret = lzma_stream_decoder(&strm, UINT64_MAX, 0);
    if (ret != LZMA_OK)
        setLzmaErr("lzma_stream_decoder", ret);
}

lzmastream::~lzmastream() {
    delete source;
}

bool lzmastream::ended() {
    return output_ended || error();
}

size_t lzmastream::read(uint8_t *buf, size_t n) {
    if (output_ended) return 0;

    lzma_action action = source->ended() ? LZMA_FINISH : LZMA_RUN;
    size_t begin_total = strm.total_out;

    strm.next_out = buf;
    strm.avail_out = n;
    while (strm.avail_out > 0) {
        if (output_ended) break;

        if (strm.avail_in == 0) {
            size_t read_bytes = source->read(in_buf, XZ_READ_BY);
            if (inheritError(source)) return 0;
            action = read_bytes == 0 ? LZMA_FINISH : LZMA_RUN;

            strm.next_in = in_buf;
            strm.avail_in = read_bytes;
        }

        lzma_ret ret = lzma_code(&strm, action);
        if (ret == LZMA_STREAM_END) {
            output_ended = true;
            lzma_end(&strm);
        }
        else if (ret != LZMA_OK) {
            setLzmaErr("lzma_code", ret);
            return 0;
        }
    }

    return strm.total_out - begin_total;
}

size_t lzmastream::sizeHint() const {
    return size_hint;
}

const char *lzma_ret_str(lzma_ret ret) {
    switch (ret) {
    case LZMA_STREAM_END:
        return "end of stream";
    case LZMA_NO_CHECK:
        return "input stream has no integrity check";
    case LZMA_UNSUPPORTED_CHECK:
        return "cannot calculate the integrity check";
    case LZMA_GET_CHECK:
        return "integrity check available";
    case LZMA_MEM_ERROR:
        return "cannot allocate memory";
    case LZMA_MEMLIMIT_ERROR:
        return "memory usage limit exceeded";
    case LZMA_FORMAT_ERROR:
        return "file format not recognized";
    case LZMA_OPTIONS_ERROR:
        return "invalid or unsupported options";
    case LZMA_DATA_ERROR:
        return "data is corrupt";
    case LZMA_BUF_ERROR:
        return "no progress is possible";
    case LZMA_PROG_ERROR:
        return "programming error";
    default:
        return "unknown";
    }
}

void lzmastream::setLzmaErr(const char *msg, lzma_ret ret)
{
    char buf[512];
    snprintf(buf, 512, "lzmastream %s: %s (%d)\n", msg, lzma_ret_str(ret), ret);
    last_error.assign(buf);
    last_errnum = ret;
}