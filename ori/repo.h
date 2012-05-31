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

#ifndef __REPO_H__
#define __REPO_H__

#include <stdint.h>

#include <string>
#include <map>
#include <set>

#define ORI_PATH_DIR "/.ori"
#define ORI_PATH_VERSION "/.ori/version"
#define ORI_PATH_UUID "/.ori/id"
#define ORI_PATH_DIRSTATE "/.ori/dirstate"
#define ORI_PATH_LOG "/.ori/ori.log"
#define ORI_PATH_TMP "/.ori/tmp/"
#define ORI_PATH_OBJS "/.ori/objs/"

class TreeEntry
{
public:
    TreeEntry();
    ~TreeEntry();
    enum EntryType {
	Null,
	Blob,
	Tree
    };
    EntryType	type;
    uint16_t	mode;
    std::string hash;
};

class Tree
{
public:
    Tree();
    ~Tree();
    void addObject(const char *path, const std::string &objId);
    const std::string getBlob();
private:
    std::map<std::string, TreeEntry> tree;
};

class Commit
{
public:
    Commit();
    ~Commit();
    void setParents(std::string p1, std::string p2 = "");
    std::set<std::string> getParents();
    void setMessage(std::string msg);
    std::string getMessage() const;
    void setTree(std::string &tree);
    std::string getTree() const;
    const std::string getBlob();
private:
};

class Repo
{
public:
    Repo();
    ~Repo();
    bool open();
    void close();
    void save();
    // Object Operations
    std::string addFile(const std::string &path);
    std::string addBlob(const std::string &blob);
    std::string addTree(/* const */ Tree &tree);
    std::string addCommit(const std::string &commit);
    char *getObject(const std::string &objId);
    size_t getObjectLength(const char *objId);
    size_t sendObject(const char *objId);
    bool copyObject(const char *objId, const char *path);
    std::set<std::string> getObjects();
    // General Operations
    std::string getUUID();
    std::string getVersion();
    static std::string getRootPath();
    static std::string getLogPath();
    static std::string getTmpFile();
private:
    std::string id;
    std::string version;
};

extern Repo repository;

#endif /* __REPO_H__ */

