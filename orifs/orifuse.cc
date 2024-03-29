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

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cinttypes>

#include <errno.h>
#include <fcntl.h>

#include <unistd.h>
#include <sys/param.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <dirent.h>
#include <pwd.h>

#include <getopt.h>

#define FUSE_USE_VERSION 26
#include <fuse.h>

#include <string>
#include <map>
#include <memory>

#include <oriutil/debug.h>
#include <oriutil/oriutil.h>
#include <oriutil/orifile.h>
#include <oriutil/systemexception.h>
#include <oriutil/rwlock.h>
#include <ori/repostore.h>
#include <ori/version.h>
#include <ori/commit.h>
#include <ori/localrepo.h>
#include <ori/remoterepo.h>

#include "logging.h"
#include "oricmd.h"
#include "oripriv.h"
#include "oriopt.h"

#ifdef DEBUG
#define FSCK_A_LOT
#endif

using namespace std;

#define ORI_CONTROL_FILENAME ".ori_control"
#define ORI_CONTROL_FILEPATH "/" ORI_CONTROL_FILENAME
#define ORI_SNAPSHOT_DIRNAME ".snapshot"
#define ORI_SNAPSHOT_DIRPATH "/" ORI_SNAPSHOT_DIRNAME

#define OPT_KEY_CLONE_PARAM 0

mount_ori_config config;
RemoteRepo remoteRepo;
OriPriv *priv;

// Mount/Unmount

static void *
ori_init(struct fuse_conn_info *conn)
{
    FUSE_LOG("Ori Filesystem starting ...");

    // Change directories to place the orifs coredump in the repo root
    chdir(config.repoPath.c_str());

    priv->init();

    return priv;
}

static void
ori_destroy(void *userdata)
{
    OriPriv *priv = GetOriPriv();
    Commit c;
    c.setMessage("FUSE snapshot on unmount");
    priv->commit(c);
    priv->cleanup();
    delete priv;

    FUSE_LOG("File system unmounted");
}

// File Manipulation

static int
ori_mknod(const char *path, mode_t mode, dev_t dev)
{
    return -EPERM;
}

static int
ori_unlink(const char *path)
{
    OriPriv *priv = GetOriPriv();

#ifdef FSCK_A_LOT
    priv->fsck();
#endif /* FSCK_A_LOT */

    FUSE_LOG("FUSE ori_unlink(path=\"%s\")", path);

    if (strcmp(path, ORI_CONTROL_FILEPATH) == 0) {
        return -EACCES;
    } else if (strncmp(path,
                ORI_SNAPSHOT_DIRPATH,
                strlen(ORI_SNAPSHOT_DIRPATH)) == 0) {
        return -EACCES;
    }

    RWKey::sp lock = priv->nsLock.writeLock();
    try {
        OriFileInfo *info = priv->getFileInfo(path);

        if (info->isDir())
            return -EPERM;

        // Remove temporary file
        if (info->path != "")
            unlink(info->path.c_str());

        if (info->isReg() || info->isSymlink()) {
            priv->unlink(path);
        } else {
            // XXX: Support files
            ASSERT(false);
        }
    } catch (SystemException e) {
        return -e.getErrno();
    }

    priv->journal("unlink", path);

    return 0;
}

static int
ori_symlink(const char *target_path, const char *link_path)
{
    OriPriv *priv = GetOriPriv();
    OriDir *parentDir;
    string parentPath;

#ifdef FSCK_A_LOT
    priv->fsck();
#endif /* FSCK_A_LOT */

    FUSE_LOG("FUSE ori_symlink(path=\"%s\")", link_path);

    parentPath = OriFile_Dirname(link_path);
    if (parentPath == "")
        parentPath = "/";

    if (strcmp(link_path, ORI_CONTROL_FILEPATH) == 0) {
        return -EACCES;
    } else if (strncmp(link_path,
                ORI_SNAPSHOT_DIRPATH,
                strlen(ORI_SNAPSHOT_DIRPATH)) == 0) {
        return -EACCES;
    }

    RWKey::sp lock = priv->nsLock.writeLock();
    try {
        parentDir = priv->getDir(parentPath);
    } catch (SystemException e) {
        return -e.getErrno();
    }

    OriFileInfo *info = priv->addSymlink(link_path);
    info->statInfo.st_mode |= 0755;
    info->link = target_path;
    info->statInfo.st_size = info->path.length();
    info->type = FILETYPE_DIRTY;

    parentDir->add(OriFile_Basename(link_path), info->id);

    return 0;
}

static int
ori_readlink(const char *path, char *buf, size_t size)
{
    OriPriv *priv = GetOriPriv();
    OriFileInfo *info;

#ifdef FSCK_A_LOT
    priv->fsck();
#endif /* FSCK_A_LOT */

    FUSE_LOG("FUSE ori_readlink(path\"%s\", size=%ld)", path, size);

    RWKey::sp lock = priv->nsLock.readLock();
    try {
        info = priv->getFileInfo(path);
    } catch (SystemException e) {
        return -e.getErrno();
    }

    memcpy(buf, info->link.c_str(), MIN(info->link.length() + 1, size));

    return 0;
}

static int
ori_rename(const char *from_path, const char *to_path)
{
    OriPriv *priv = GetOriPriv();

#ifdef FSCK_A_LOT
    priv->fsck();
#endif /* FSCK_A_LOT */

    FUSE_LOG("FUSE ori_rename(from_path=\"%s\", to_path=\"%s\")",
             from_path, to_path);

    if (strncmp(to_path,
                ORI_SNAPSHOT_DIRPATH,
                strlen(ORI_SNAPSHOT_DIRPATH)) == 0) {
        return -EACCES;
    }
    if (strncmp(from_path,
                ORI_SNAPSHOT_DIRPATH,
                strlen(ORI_SNAPSHOT_DIRPATH)) == 0) {
        return -EACCES;
    }

    RWKey::sp lock = priv->nsLock.writeLock();
    try {
        OriFileInfo *info = priv->getFileInfo(from_path);
        OriFileInfo *toFile = NULL;
        OriDir *toFileDir = NULL;

        try {
            toFile = priv->getFileInfo(to_path);
        } catch (SystemException e) {
            // Fall through
        }

        // Not sure if FUSE checks for these two error cases
        if (toFile != NULL && toFile->isDir()) {
            toFileDir = priv->getDir(to_path);

            if (!toFileDir->isEmpty())
                return -ENOTEMPTY;
        }
        if (toFile != NULL && info->isDir() && !toFile->isDir()) {
            return -EISDIR;
        }

        // XXX: Need to support renaming directories (nlink, OriPriv::Rename)
        if (info->isDir()) {
            FUSE_LOG("ori_rename: Directory rename attempted %s to %s",
                     from_path, to_path);
            return -EINVAL;
        }

        priv->rename(from_path, to_path);
    } catch (SystemException &e) {
        return -e.getErrno();
    }

    string journalArg = from_path;
    journalArg += ":";
    journalArg += to_path;
    priv->journal("rename", journalArg);

    return 0;
}

// File IO

static int
ori_create(const char *path, mode_t mode, struct fuse_file_info *fi)
{
    OriPriv *priv = GetOriPriv();
    string parentPath;
    OriDir *parentDir;

#ifdef FSCK_A_LOT
    priv->fsck();
#endif /* FSCK_A_LOT */

    FUSE_LOG("FUSE ori_create(path=\"%s\")", path);

    parentPath = OriFile_Dirname(path);
    if (parentPath == "")
        parentPath = "/";

    if (strncmp(path,
                ORI_SNAPSHOT_DIRPATH,
                strlen(ORI_SNAPSHOT_DIRPATH)) == 0) {
        return -EACCES;
    }

    RWKey::sp lock = priv->nsLock.writeLock();
    try {
        parentDir = priv->getDir(parentPath);
    } catch (SystemException e) {
        return -e.getErrno();
    }

    pair<OriFileInfo *, uint64_t> info = priv->addFile(path);
    info.first->statInfo.st_mode |= mode;
    info.first->type = FILETYPE_DIRTY;

    parentDir->add(OriFile_Basename(path), info.first->id);

    string journalArg = path;
    journalArg += ":" + info.first->path;
    priv->journal("create", journalArg);

    // Set fh
    fi->fh = info.second;

    return 0;
}

static int
ori_open(const char *path, struct fuse_file_info *fi)
{
    OriPriv *priv = GetOriPriv();
    string parentPath;
    OriDir *parentDir;
    pair<OriFileInfo *, uint64_t> info;
    bool writing = false;
    bool trunc = false;
    
    if (fi->flags & O_WRONLY || fi->flags & O_RDWR)
        writing = true;
    if (fi->flags & O_TRUNC)
        trunc = true;

    FUSE_LOG("FUSE ori_open(path=\"%s\")", path);

    if (strcmp(path, ORI_CONTROL_FILEPATH) == 0) {
        return 0;
    } else if (strncmp(path,
                       ORI_SNAPSHOT_DIRPATH,
                       strlen(ORI_SNAPSHOT_DIRPATH)) == 0) {
        return writing ? -EPERM : 0;
    }

    parentPath = OriFile_Dirname(path);
    if (parentPath == "")
        parentPath = "/";

    RWKey::sp lock = priv->nsLock.writeLock();
    try {
        parentDir = priv->getDir(parentPath);
        info = priv->openFile(path, /*writing*/writing, /*trunc*/trunc);
    } catch (SystemException e) {
        return -e.getErrno();
    }

    if (writing)
        parentDir->setDirty();

    // Set fh
    fi->fh = info.second;

    return 0;
}

static int
ori_read(const char *path, char *buf, size_t size, off_t offset,
         struct fuse_file_info *fi)
{
    OriPriv *priv = GetOriPriv();
    OriFileInfo *info;
    int status;

    // FUSE_LOG("FUSE ori_read(path=\"%s\", length=%ld, offset=%ld)",
    //          path, size, offset);

    if (strcmp(path, ORI_CONTROL_FILEPATH) == 0) {
        string repoPath = priv->getRepo()->getRootPath();
        int len = (size < repoPath.size()) ? size : repoPath.size();
        if (offset != 0 || size < repoPath.size())
            return -EIO;
        memcpy(buf, repoPath.data(), len);
        return len;
    } else if (strncmp(path,
                       ORI_SNAPSHOT_DIRPATH,
                       strlen(ORI_SNAPSHOT_DIRPATH)) == 0) {
        string snapshot = path;
        string parentPath, fileName;
        size_t pos = 0;
        Commit c;
        Tree t;
        
        snapshot = snapshot.substr(strlen(ORI_SNAPSHOT_DIRPATH) + 1);
        pos = snapshot.find('/', pos);

        ASSERT(pos != snapshot.npos);

        parentPath = snapshot.substr(pos);
        snapshot = snapshot.substr(0, pos);
        fileName = OriFile_Basename(parentPath);
        parentPath = OriFile_Dirname(parentPath);
        if (parentPath == "")
            parentPath = "/";

        // XXX: Enforce that this is a valid snapshot & directory path
        c = priv->lookupSnapshot(snapshot);
        t = priv->getTree(c, parentPath);

        // lookup tree
        Tree::iterator it = t.find(fileName);
        if (it == t.end())
            return -ENOENT;

        // Read
        OriFileInfo *tempInfo = new OriFileInfo();
        tempInfo->type = FILETYPE_COMMITTED;
        tempInfo->hash = it->second.hash;
        status = priv->readFile(tempInfo, buf, size, offset);
        tempInfo->release();
        return status;
    }

    RWKey::sp lock = priv->nsLock.readLock();
    info = priv->getFileInfo(fi->fh);

    // Return an error when reading from a directory
    if (info->isDir()) {
        return -EISDIR;
    }

    if (info->fd != -1) {
        // File in temporary directory
        status = pread(info->fd, buf, size, offset);
        if (status < 0)
            return -errno;
    } else {
        // File in repository
        return priv->readFile(info, buf, size, offset);
    }

    return status;
}

static int
ori_write(const char *path, const char *buf, size_t size, off_t offset,
         struct fuse_file_info *fi)
{
    OriPriv *priv = GetOriPriv();
    OriFileInfo *info;
    int status;

    // FUSE_LOG("FUSE ori_write(path=\"%s\", length=%ld)", path, size);

    if (strcmp(path, ORI_CONTROL_FILEPATH) == 0) {
        return -EIO;
    } else if (strncmp(path,
                       ORI_SNAPSHOT_DIRPATH,
                       strlen(ORI_SNAPSHOT_DIRPATH)) == 0) {
        ASSERT(false);
        return -EIO;
    }

    RWKey::sp lock = priv->nsLock.readLock();
    info = priv->getFileInfo(fi->fh);

    // Return an error on a directory write
    if (info->isDir()) {
        return -EISDIR;
    }

    info->type = FILETYPE_DIRTY;
    status = pwrite(info->fd, buf, size, offset);
    if (status < 0)
        return -errno;

    // Update size
    if (info->statInfo.st_size < (off_t)size + offset) {
        info->statInfo.st_size = size + offset;
        info->statInfo.st_blocks = (size + offset + (512-1))/512;
    }

    return status;
}

static int
ori_truncate(const char *path, off_t length)
{
    OriPriv *priv = GetOriPriv();
    OriFileInfo *info;

    FUSE_LOG("FUSE ori_truncate(path=\"%s\", length=%" PRId64 ")", path, length);

    if (strcmp(path, ORI_CONTROL_FILEPATH) == 0) {
        return -EACCES;
    } else if (strncmp(path,
                       ORI_SNAPSHOT_DIRPATH,
                       strlen(ORI_SNAPSHOT_DIRPATH)) == 0) {
        return -EACCES;
    }

    RWKey::sp lock = priv->nsLock.writeLock();
    info = priv->getFileInfo(path);
    if (info->type == FILETYPE_DIRTY) {
        int status;

        status = truncate(info->path.c_str(), length);
        if (status < 0)
            return -errno;

        // Update size
        info->statInfo.st_size = length;
        info->statInfo.st_blocks = (length + (512-1))/512;

        return status;
    } else {
        // XXX: Not Implemented
        ASSERT(false);
        return -EINVAL;
    }
}

static int
ori_ftruncate(const char *path, off_t length, struct fuse_file_info *fi)
{
    OriPriv *priv = GetOriPriv();
    OriFileInfo *info;

    FUSE_LOG("FUSE ori_ftruncate(path=\"%s\", length=%" PRId64 ")", path, length);

    if (strcmp(path, ORI_CONTROL_FILEPATH) == 0) {
        return -EACCES;
    } else if (strncmp(path,
                       ORI_SNAPSHOT_DIRPATH,
                       strlen(ORI_SNAPSHOT_DIRPATH)) == 0) {
        ASSERT(false);
        return -EIO;
    }

    RWKey::sp lock = priv->nsLock.writeLock();
    info = priv->getFileInfo(fi->fh);
    if (info->type == FILETYPE_DIRTY) {
        int status;

        status = ftruncate(info->fd, length);
        if (status < 0)
            return -errno;

        // Update size
        info->statInfo.st_size = length;
        info->statInfo.st_blocks = (length + (512-1))/512;

        return status;
    } else {
        // XXX: Not Implemented
        ASSERT(false);
        return -EINVAL;
    }
}

static int
ori_release(const char *path, struct fuse_file_info *fi)
{
    OriPriv *priv = GetOriPriv();

    FUSE_LOG("FUSE ori_release(path=\"%s\"): fh=%" PRIu64, path, fi->fh);

    if (strcmp(path, ORI_CONTROL_FILEPATH) == 0) {
        return 0;
    } else if (strncmp(path,
                       ORI_SNAPSHOT_DIRPATH,
                       strlen(ORI_SNAPSHOT_DIRPATH)) == 0) {
        return 0;
    }

    RWKey::sp lock = priv->nsLock.writeLock();
    // Decrement reference count (deletes temporary file for unlink)
    return priv->closeFH(fi->fh);
}

// Directory Operations

static int
ori_mkdir(const char *path, mode_t mode)
{
    OriPriv *priv = GetOriPriv();

#ifdef FSCK_A_LOT
    priv->fsck();
#endif /* FSCK_A_LOT */

    FUSE_LOG("FUSE ori_mkdir(path=\"%s\")", path);

    if (strncmp(path,
                ORI_SNAPSHOT_DIRPATH,
                strlen(ORI_SNAPSHOT_DIRPATH)) == 0) {
        return -EACCES;
    }

    RWKey::sp lock = priv->nsLock.writeLock();
    try {
        OriFileInfo *info = priv->addDir(path);
        info->statInfo.st_mode |= mode;
    } catch (SystemException e) {
        return -e.getErrno();
    }

    priv->journal("mkdir", path);

    return 0;
}

static int
ori_rmdir(const char *path)
{
    OriPriv *priv = GetOriPriv();

#ifdef FSCK_A_LOT
    priv->fsck();
#endif /* FSCK_A_LOT */

    FUSE_LOG("FUSE ori_rmdir(path=\"%s\")", path);

    if (strncmp(path,
                ORI_SNAPSHOT_DIRPATH,
                strlen(ORI_SNAPSHOT_DIRPATH)) == 0) {
        return -EACCES;
    }

    RWKey::sp lock = priv->nsLock.writeLock();
    try {
        OriDir *dir = priv->getDir(path);

        if (!dir->isEmpty()) {
            OriDir::iterator it;

            FUSE_LOG("Directory not empty!");
            for (it = dir->begin(); it != dir->end(); it++) {
                FUSE_LOG("DIR: %s\n", it->first.c_str());
            }

            return -ENOTEMPTY;
        }

        priv->rmDir(path);

    } catch (SystemException &e) {
        FUSE_LOG("ori_rmdir: Caught exception %s", e.what());
        return -e.getErrno();
    }

    priv->journal("rmdir", path);

    return 0;
}

static int
ori_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
                        off_t offset, struct fuse_file_info *fi)
{
    OriPriv *priv = GetOriPriv();
    OriDir *dir;
    OriDir::iterator it;
    string dirPath = path;

    if (dirPath != "/")
        dirPath += "/";

#ifdef FSCK_A_LOT
    priv->fsck();
#endif /* FSCK_A_LOT */

    FUSE_LOG("FUSE ori_readdir(path=\"%s\", offset=%" PRId64 ")", path, offset);

    filler(buf, ".", NULL, 0);
    filler(buf, "..", NULL, 0);
    if (strcmp(path, "/") == 0) {
        filler(buf, ORI_CONTROL_FILENAME, NULL, 0);
        filler(buf, ORI_SNAPSHOT_DIRNAME, NULL, 0);
    } else if (strcmp(path, ORI_SNAPSHOT_DIRPATH) == 0) {
        map<string, ObjectHash> snapshots = priv->listSnapshots();
        map<string, ObjectHash>::iterator it;

        for (it = snapshots.begin(); it != snapshots.end(); it++) {
            filler(buf, (*it).first.c_str(), NULL, 0);
        }

        return 0;
    } else if (strncmp(path,
                       ORI_SNAPSHOT_DIRPATH,
                       strlen(ORI_SNAPSHOT_DIRPATH)) == 0) {
        string snapshot = path;
        string relPath;
        size_t pos = 0;
        Commit c;
        Tree t;
        
        snapshot = snapshot.substr(strlen(ORI_SNAPSHOT_DIRPATH) + 1);
        pos = snapshot.find('/', pos);

        if (pos == snapshot.npos) {
            relPath = "/";
        } else {
            relPath = snapshot.substr(pos);
            snapshot = snapshot.substr(0, pos);
        }

        // XXX: Enforce that this is a valid snapshot & directory path
        c = priv->lookupSnapshot(snapshot);
        t = priv->getTree(c, relPath);

        for (map<string, TreeEntry>::iterator it = t.tree.begin();
             it != t.tree.end();
             it++) {
            filler(buf, (*it).first.c_str(), NULL, 0);
        }

        return 0;
    }

    RWKey::sp lock = priv->nsLock.writeLock();
    try {
        dir = priv->getDir(path);
    } catch (SystemException e) {
        return -e.getErrno();
    }

    for (it = dir->begin(); it != dir->end(); it++) {
        OriFileInfo *info;
        
        try {
            info = priv->getFileInfo(dirPath + (*it).first);
            filler(buf, (*it).first.c_str(), &info->statInfo, 0);
        } catch (SystemException e) {
            FUSE_LOG("Unexpected %s", e.what());
            filler(buf, (*it).first.c_str(), NULL, 0);
        }
    }

    return 0;
}


// File Attributes

static int
ori_getattr(const char *path, struct stat *stbuf)
{
    OriPriv *priv = GetOriPriv();

    FUSE_LOG("FUSE ori_getattr(path=\"%s\")", path);

    memset(stbuf, 0, sizeof(struct stat));

    if (strcmp(path, ORI_CONTROL_FILEPATH) == 0) {
        string repoPath = priv->getRepo()->getRootPath();
        stbuf->st_uid = geteuid();
        stbuf->st_gid = getegid();
        stbuf->st_mode = 0600 | S_IFREG;
        stbuf->st_nlink = 1;
        stbuf->st_size = repoPath.size();
        stbuf->st_blksize = 4096;
        stbuf->st_blocks = (stbuf->st_size + 511) / 512;
        return 0;
    } else if (strcmp(path, ORI_SNAPSHOT_DIRPATH) == 0) {
        stbuf->st_uid = geteuid();
        stbuf->st_gid = getegid();
        stbuf->st_mode = 0755 | S_IFDIR;
        stbuf->st_nlink = 2;
        stbuf->st_size = 512;
        stbuf->st_blksize = 4096;
        stbuf->st_blocks = 1;
        return 0;
    } else if (strncmp(path,
                       ORI_SNAPSHOT_DIRPATH,
                       strlen(ORI_SNAPSHOT_DIRPATH)) == 0) {
        string snapshot = path;
        string parentPath, fileName;
        size_t pos = 0;
        Commit c;
        Tree t;
        
        snapshot = snapshot.substr(strlen(ORI_SNAPSHOT_DIRPATH) + 1);
        pos = snapshot.find('/', pos);

        if (pos == snapshot.npos) {
            c = priv->lookupSnapshot(snapshot);
            stbuf->st_uid = geteuid();
            stbuf->st_gid = getegid();
            stbuf->st_mode = 0755 | S_IFDIR;
            stbuf->st_nlink = 2;
            stbuf->st_size = 512;
            stbuf->st_blksize = 4096;
            stbuf->st_blocks = 1;
            stbuf->st_ctime = c.getTime();
            stbuf->st_mtime = c.getTime();
            return 0;
        }

        parentPath = snapshot.substr(pos);
        snapshot = snapshot.substr(0, pos);
        fileName = OriFile_Basename(parentPath);
        parentPath = OriFile_Dirname(parentPath);
        if (parentPath == "")
            parentPath = "/";

        // XXX: Enforce that this is a valid snapshot & directory path
        c = priv->lookupSnapshot(snapshot);
        t = priv->getTree(c, parentPath);

        // lookup tree
        Tree::iterator it = t.find(fileName);
        if (it == t.end())
            return -ENOENT;

        // Convert
        AttrMap *attrs = &it->second.attrs;
        struct passwd *pw = getpwnam(attrs->getAsStr(ATTR_USERNAME).c_str());

        memset(stbuf, 0, sizeof(*stbuf));
        if (it->second.type == TreeEntry::Tree) {
            stbuf->st_mode = S_IFDIR;
            stbuf->st_nlink = 2; // XXX: Correct this!
        } else {
            stbuf->st_mode = S_IFREG;
            stbuf->st_nlink = 1;
        }
        stbuf->st_mode |= attrs->getAs<mode_t>(ATTR_PERMS);
        stbuf->st_uid = pw->pw_uid;
        stbuf->st_gid = pw->pw_gid;
        stbuf->st_size = attrs->getAs<size_t>(ATTR_FILESIZE);
        stbuf->st_blocks = (stbuf->st_size + 511) / 512;
        stbuf->st_mtime = attrs->getAs<time_t>(ATTR_MTIME);
        stbuf->st_ctime = attrs->getAs<time_t>(ATTR_CTIME);

        return 0;
    }

    RWKey::sp lock = priv->nsLock.writeLock();
    try {
        OriFileInfo *info = priv->getFileInfo(path);
        *stbuf = info->statInfo;
    } catch (SystemException e) {
        return -e.getErrno();
    }

    return 0;
}

static int
ori_chmod(const char *path, mode_t mode)
{
    OriPriv *priv = GetOriPriv();
    string parentPath;

    parentPath = OriFile_Dirname(path);
    if (parentPath == "")
        parentPath = "/";

    FUSE_LOG("FUSE ori_chmod(path=\"%s\")", path);

    if (strcmp(path, ORI_CONTROL_FILEPATH) == 0) {
        return -EACCES;
    } else if (strncmp(path,
                ORI_SNAPSHOT_DIRPATH,
                strlen(ORI_SNAPSHOT_DIRPATH)) == 0) {
        return -EACCES;
    }

    RWKey::sp lock = priv->nsLock.writeLock();
    try {
        OriFileInfo *info = priv->getFileInfo(path);

        info->statInfo.st_mode = mode;
        info->type = FILETYPE_DIRTY;

        OriDir *dir = priv->getDir(parentPath);
        dir->setDirty();
    } catch (SystemException e) {
        return -e.getErrno();
    }

    return 0;
}

static int
ori_chown(const char *path, uid_t uid, gid_t gid)
{
    OriPriv *priv = GetOriPriv();
    string parentPath;

    parentPath = OriFile_Dirname(path);
    if (parentPath == "")
        parentPath = "/";

    FUSE_LOG("FUSE ori_chmod(path=\"%s\")", path);

    if (strcmp(path, ORI_CONTROL_FILEPATH) == 0) {
        return -EACCES;
    } else if (strncmp(path,
                ORI_SNAPSHOT_DIRPATH,
                strlen(ORI_SNAPSHOT_DIRPATH)) == 0) {
        return -EACCES;
    }

    RWKey::sp lock = priv->nsLock.writeLock();
    try {
        OriFileInfo *info = priv->getFileInfo(path);

        info->statInfo.st_uid = uid;
        info->statInfo.st_gid = gid;
        info->type = FILETYPE_DIRTY;

        OriDir *dir = priv->getDir(parentPath);
        dir->setDirty();
    } catch (SystemException e) {
        return -e.getErrno();
    }

    return 0;
}

static int
ori_utimens(const char *path, const struct timespec tv[2])
{
    OriPriv *priv = GetOriPriv();
    string parentPath;

    parentPath = OriFile_Dirname(path);
    if (parentPath == "")
        parentPath = "/";

    FUSE_LOG("FUSE ori_utimens(path=\"%s\")", path);

    if (strcmp(path, ORI_CONTROL_FILEPATH) == 0) {
        return -EACCES;
    } else if (strncmp(path,
                ORI_SNAPSHOT_DIRPATH,
                strlen(ORI_SNAPSHOT_DIRPATH)) == 0) {
        return -EACCES;
    }

    RWKey::sp lock = priv->nsLock.writeLock();
    try {
        OriFileInfo *info = priv->getFileInfo(path);

        // Ignore access times
        info->statInfo.st_mtime = tv[1].tv_sec;
        info->type = FILETYPE_DIRTY;

        OriDir *dir = priv->getDir(parentPath);
        dir->setDirty();
    } catch (SystemException e) {
        return -e.getErrno();
    }

    return 0;
}

static int
ori_fsync(const char *path, int isdatasync, struct fuse_file_info *fi)
{
    OriFileInfo *info;

    if (strcmp(path, ORI_CONTROL_FILEPATH) == 0) {
        return 0;
    } else if (strncmp(path,
                       ORI_SNAPSHOT_DIRPATH,
                       strlen(ORI_SNAPSHOT_DIRPATH)) == 0) {
        return -EBADF;
    }

    RWKey::sp lock = priv->nsLock.readLock();
    try {
        info = priv->getFileInfo(fi->fh);
        if (info->fd == -1)
            return 0; // XXX: File is closed ignore

        return fsync(info->fd);
    } catch (SystemException &e) {
        // Fall through
    }

    return 0;
}

static struct fuse_operations ori_oper;

static void
ori_setup_ori_oper()
{
    memset(&ori_oper, 0, sizeof(struct fuse_operations));
    ori_oper.create = ori_create;

    ori_oper.init = ori_init;
    ori_oper.destroy = ori_destroy;

    ori_oper.mknod = ori_mknod;
    ori_oper.unlink = ori_unlink;
    ori_oper.symlink = ori_symlink;
    ori_oper.readlink = ori_readlink;
    ori_oper.rename = ori_rename;

    ori_oper.open = ori_open;
    ori_oper.read = ori_read;
    ori_oper.write = ori_write;
    ori_oper.truncate = ori_truncate;
    ori_oper.ftruncate = ori_ftruncate;
    ori_oper.release = ori_release;

    ori_oper.mkdir = ori_mkdir;
    ori_oper.rmdir = ori_rmdir;
    ori_oper.readdir = ori_readdir;

    ori_oper.getattr = ori_getattr;
    // XXX: fgetattr
    ori_oper.chmod = ori_chmod;
    ori_oper.chown = ori_chown;
    ori_oper.utimens = ori_utimens;

    ori_oper.fsync = ori_fsync;
    // XXX: lock (for DLM)
}

void
version()
{
    printf("Ori Distributed Personal File System (%s) - FUSE Driver\n",
            ORI_VERSION_STR);
}

void
usage()
{
    version();
    printf("Usage: orifs [REPOSITORY] [MOUNT POINT] [-o OPTIONS] [--help]\n\n");
    printf("The repository may be a fully qualified repository path, or a\n");
    printf("local repository name.\n");
    printf("\nOri mount options:\n");
    printf("    -o clone=[REMOTE PATH]          Clone remote repository into\n");
    printf("                                    the local repository path.\n");
    printf("    -o cache=[none,shallow,deep]    Disable caching of clone, or\n");
    printf("                                    force shallow caching. Default\n");
    printf("                                    is 'deep'.\n");
    printf("    -o journal=[none,async,sync]    Disable recovery journal,\n");
    printf("                                    or use a synchronous or\n");
    printf("                                    asynchronous journal. Default\n");
    printf("                                    is 'async'.\n");
    printf("\nOther mount options will be passed on to FUSE; see below.\n");

    printf("\nPlease report bugs to orifs-devel@stanford.edu\n");
    printf("Website: http://ori.scs.stanford.edu/\n\n");
}


const fuse_opt option_spec[] = {
  // Respond to standard options
  { "--help", offsetof(struct mount_ori_config, show_help), 1 },
  { "-h", offsetof(struct mount_ori_config, show_help), 1 },

  { "--version", offsetof(struct mount_ori_config, show_version), 1 },
  { "-V", offsetof(struct mount_ori_config, show_version), 1 },

  // File system options
  { "cache=none", offsetof(struct mount_ori_config, cache), (int) OriCacheMode::None },
  { "no_cache", offsetof(struct mount_ori_config, cache), (int) OriCacheMode::None },
  { "cache=shallow", offsetof(struct mount_ori_config, cache), (int) OriCacheMode::Shallow },
  { "cache=deep", offsetof(struct mount_ori_config, cache), (int) OriCacheMode::Deep },

  { "journal=none", offsetof(struct mount_ori_config, journal), (int) OriJournalMode::NoJournal },
  { "no_journal", offsetof(struct mount_ori_config, journal), (int) OriJournalMode::NoJournal },
  { "journal=async", offsetof(struct mount_ori_config, journal), (int) OriJournalMode::AsyncJournal },
  { "journal=sync", offsetof(struct mount_ori_config, journal), (int) OriJournalMode::SyncJournal },

  { "-s", offsetof(struct mount_ori_config, single), 1 },

  { "-d", offsetof(struct mount_ori_config, debug), 1 },
  { "debug", offsetof(struct mount_ori_config, debug), 1 },
  { "no_debug", offsetof(struct mount_ori_config, debug), 0 },

  { "clone=", -1U, OPT_KEY_CLONE_PARAM },

  FUSE_OPT_END
};

int set_opt_helper(void * data, char const * arg, int key, struct fuse_args *out)
{
  // Not recognized, so let FUSE handle it
  if (key == FUSE_OPT_KEY_OPT) {
    return 1;
  }

  // Just for clean code, don't depend on global - we have the options passed
  // in.
  mount_ori_config * cbopts = static_cast<mount_ori_config *>(data);

  // Non-option argument
  if (key == FUSE_OPT_KEY_NONOPT) {
    switch (cbopts->argcount) {
      case 0:
        // First argument must be the repository. We'll remember this, but
        // discard it from the args.
        cbopts->repoPath = arg;
        ++(cbopts->argcount);
        return 0;

      case 1:
        // The second argument must be the mount point. We'll remember it,
        // but preserve it for FUSE.
        cbopts->mountPoint = arg;
        ++(cbopts->argcount);
        return 1;

      default:
        // Any future arguments are errors.
        fprintf(stderr, "Internal error: too many arguments: %s\n", arg);
        return -1;
    }
  }

  switch (key) {
    case OPT_KEY_CLONE_PARAM:
      {
        std::string clone = arg;
        if (clone.substr(0, 6) != "clone=") {
          fprintf(stderr, "Internal error: clone option does not start with 'clone=': %s\n", arg);
          return -1;
        }
        cbopts->clonePath = clone.substr(5);
      }
      break;

    default:
      // We don't know it, error out.
      fprintf(stderr, "Internal error: unrecognized option: %s\n", arg);
      return -1;
  }

  // Discard from args, it's handled
  return 0;
}



int
main(int argc, char *argv[])
{
    int ch;
    bool createReplica = false;

    ori_setup_ori_oper();

    fuse_args args = FUSE_ARGS_INIT(argc, argv);

    int ret = fuse_opt_parse(&args, &config, option_spec, set_opt_helper);
    if (ret == -1) {
      usage();
      assert(fuse_opt_add_arg(&args, "--help") == 0);
      args.argv[0][0] = '\0';
    }

    // Handle generic options
    if (config.show_help) {
      usage();
      ret = fuse_opt_add_arg(&args, "--help");
      assert(ret == 0);
      args.argv[0][0] = '\0';
    } else if (config.show_version) {
      version();
      ret = fuse_opt_add_arg(&args, "--version");
      assert(ret == 0);
      args.argv[0][0] = '\0';
    }

    // Pass on FUSE options that might have been consumed.
    if (config.debug) {
      ret = fuse_opt_add_arg(&args, "-d");
      assert(ret == 0);
      ori_fuse_log_enable();
    }
    if (config.single) {
      ret = fuse_opt_add_arg(&args, "-s");
      assert(ret == 0);
    }

    // If we have a clone path, we need to create a replica.

    // If we have a clone path, we need to create a replica.
    if (config.clonePath.length() > 0) {
      createReplica = true;
    }

    // If we want to show version or help, it's best to do this fast.
    if (config.show_version || config.show_help) {
          int ret = fuse_main(args.argc, args.argv, &ori_oper, NULL);
      fuse_opt_free_args(&args);
      return ret;
    }

    // If there is no mount point use the repoPath as the mountpoint
    if (config.mountPoint == "" && config.repoPath != "" && !createReplica) {
        config.mountPoint = OriFile_Basename(config.repoPath);
        fuse_opt_add_arg(&args, config.mountPoint.c_str());
        if (!OriFile_Exists(OriFile_Basename(config.mountPoint))) {
            OriFile_MkDir(OriFile_Basename(config.mountPoint));
        }
    }
    /*
     * If there is no repo path, then check if the repository name is the 
     * mountpoint name.  Otherwise we will generate it form the clone path.
     */
    if (config.repoPath == "" && createReplica) {
        string fsName = config.clonePath;
        fsName = fsName.substr(fsName.rfind("/") + 1);
        if (fsName == config.clonePath)
            fsName = fsName.substr(fsName.rfind(":") + 1);
        config.repoPath = RepoStore_GetRepoPath(fsName);
    }

    config.repoPath = RepoStore_FindRepo(config.repoPath);
    if (config.repoPath != "" && !OriFile_Exists(config.repoPath)) {
        printf("Specify the repository name or repository path!\n");
        fuse_opt_free_args(&args);
        exit(1);
    }

    /*
     * Now we do the real work of replicating and mounting the file system.
     */

#if defined(DEBUG) || defined(ORI_PERF)
    ori_fuse_log_enable();
#endif

    FUSE_PLOG("Ori FUSE Driver");

    if (createReplica) {
        FUSE_LOG("InstaCloning from %s", config.clonePath.c_str());
    }
    FUSE_PLOG("Opening repo at %s", config.repoPath.c_str());

    if (!OriFile_Exists(config.repoPath) && !createReplica) {
        printf("Repository does not exist! You must create one with ");
        printf("'ori init', or you may\nreplicate one from another host!\n");
        fuse_opt_free_args(&args);
        return 1;
    }

    if (createReplica) {
        if (OriFile_Exists(config.repoPath)) {
            printf("Cannot replicate onto an existing file system!\n");
            fuse_opt_free_args(&args);
            return 1;
        }

        int status = OriFile_MkDir(config.repoPath);
        if (status < 0) {
            printf("Failed to destination repository directory!\n");
            fuse_opt_free_args(&args);
            return 1;
        }

        config.repoPath = OriFile_RealPath(config.repoPath);

        printf("Creating new repository %s\n", config.repoPath.c_str());
        if (!remoteRepo.connect(config.clonePath)) {
            printf("Failed to connect to remote repository: %s\n",
                   config.clonePath.c_str());
            fuse_opt_free_args(&args);
            return 1;
        }

        if (LocalRepo_Init(config.repoPath, /* bareRepo */true,
                           remoteRepo->getUUID()) != 0) {
            printf("Repository does not exist and failed to create one.\n");
            fuse_opt_free_args(&args);
            return 1;
        }

        FUSE_LOG("InstaClone: Enabled!");
    }
    config.repoPath = OriFile_RealPath(config.repoPath);

    if (config.cache == OriCacheMode::Deep && createReplica) {
        try {
            LocalRepo repo;

            NOT_IMPLEMENTED(false);
            repo.open(config.repoPath);
            repo.setHead(remoteRepo->getHead());
            repo.pull(remoteRepo.get());
            repo.close();
        } catch (SystemException &e) {
            FUSE_LOG("Unexpected %s", e.what());
            fuse_opt_free_args(&args);
            throw e;
        }
    }

    try {
        if (config.cache == OriCacheMode::Shallow && createReplica) {
            string originPath = config.clonePath;

            if (!Util_IsPathRemote(originPath)) {
                originPath = OriFile_RealPath(originPath);
            }

            priv = new OriPriv(config.repoPath, originPath, remoteRepo.get());
        } else {
            priv = new OriPriv(config.repoPath);
        }
    } catch (SystemException e) {
        FUSE_LOG("Unexpected %s", e.what());
        fuse_opt_free_args(&args);
        throw e;
    }

    // Cast is safe enough in C++, if code explicitly sets and tests for
    // enum values - which it does.
    priv->setJournalMode(static_cast<OriJournalMode::JournalMode>(config.journal));

    if (config.debug == 1) {
        cout << "Repo Path:     " << config.repoPath << endl;
        cout << "Clone Path:    " << config.clonePath << endl;
        cout << "Mount Point:   " << config.mountPoint << endl;
    }

    int status = fuse_main(args.argc, args.argv, &ori_oper, NULL);
    if (status != 0) {
        priv->cleanup();
    }

    fuse_opt_free_args(&args);
    return status;
}

