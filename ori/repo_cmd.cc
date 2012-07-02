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

#define _WITH_DPRINTF

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>

#include <unistd.h>
#include <sys/param.h>
#include <sys/types.h>
#include <sys/stat.h>

#include <string>
#include <iostream>
#include <iomanip>

#include "debug.h"
#include "scan.h"
#include "util.h"
#include "repo.h"
#include "sshclient.h"
#include "sshrepo.h"

using namespace std;

/********************************************************************
 *
 *
 * Commands
 *
 *
 ********************************************************************/


int
cmd_init(int argc, const char *argv[])
{
    string rootPath;
    string oriDir;
    string tmpDir;
    string objDir;
    string versionFile;
    string uuidFile;
    int fd;
    
    if (argc == 1) {
        char *cwd = getcwd(NULL, MAXPATHLEN);
        rootPath = cwd;
        free(cwd);
    } else if (argc == 2) {
        rootPath = argv[1];
	if (!Util_IsDirectory(rootPath)) {
	    mkdir(rootPath.c_str(), 0755);
	}
    } else {
        printf("Too many arguments!\n");
        return 1;
    }

    // Create directory
    oriDir = rootPath + ORI_PATH_DIR;
    if (mkdir(oriDir.c_str(), 0755) < 0) {
        perror("Could not create '.ori' directory");
        return 1;
    }

    // Create tmp directory
    tmpDir = rootPath + ORI_PATH_DIR + "/tmp";
    if (mkdir(tmpDir.c_str(), 0755) < 0) {
        perror("Could not create '.ori/tmp' directory");
        return 1;
    }

    // Create objs directory
    tmpDir = rootPath + ORI_PATH_DIR + "/objs";
    if (mkdir(tmpDir.c_str(), 0755) < 0) {
        perror("Could not create '.ori/objs' directory");
        return 1;
    }

    // Construct UUID
    uuidFile = rootPath + ORI_PATH_UUID;
    fd = open(uuidFile.c_str(), O_CREAT|O_WRONLY|O_APPEND, 0660);
    if (fd < 0) {
        perror("Could not create UUID file");
        return 1;
    }

    std::string generated_uuid = Util_NewUUID();
    write(fd, generated_uuid.data(), generated_uuid.length());
    close(fd);
    chmod(uuidFile.c_str(), 0440);

    // Construct version file
    versionFile = rootPath + ORI_PATH_VERSION;
    fd = open(versionFile.c_str(), O_CREAT|O_WRONLY|O_APPEND, 0660);
    if (fd < 0) {
        perror("Could not create version file");
        return 1;
    }
    dprintf(fd, "ORI1.0");
    close(fd);

    return 0;
}

int
cmd_show(int argc, const char *argv[])
{
    string rootPath = Repo::findRootPath();

    if (rootPath.compare("") == 0) {
        printf("No repository found!\n");
        return 1;
    }

    printf("--- Repository ---\n");
    printf("Root: %s\n", rootPath.c_str());
    printf("UUID: %s\n", repository.getUUID().c_str());
    printf("Version: %s\n", repository.getVersion().c_str());
    printf("HEAD: %s\n", repository.getHead().c_str());
    //printf("Peers:\n");
    // for
    // printf("    %s\n", hostname);
    return 0;
}

int
cmd_catobj(int argc, const char *argv[])
{
    size_t len;
    const unsigned char *rawBuf;
    std::string buf;
    bool hex = false;

    len = repository.getObjectLength(argv[1]);
    if (len == -1) {
	printf("Object does not exist.\n");
	return 1;
    }

    buf = repository.getPayload(argv[1]);
    rawBuf = (const unsigned char *)buf.data();

    for (int i = 0; i < len; i++) {
        if (rawBuf[i] < 1 || rawBuf[i] >= 0x80) {
            hex = true;
            break;
        }
    }

    if (hex) {
        printf("Hex Dump (%ld bytes):\n", len);
        Util_PrintHex(buf, 0, len);
        printf("\n");
    } else {
        printf("%s", rawBuf);
    }

    return 0;
}

int
cmd_listobj(int argc, const char *argv[])
{
    set<string> objects = repository.listObjects();
    set<string>::iterator it;

    for (it = objects.begin(); it != objects.end(); it++)
    {
	const char *type;
	switch (repository.getObjectType(*it))
	{
	    case Object::Commit:
		type = "Commit";
		break;
	    case Object::Tree:
		type = "Tree";
		break;
	    case Object::Blob:
		type = "Blob";
		break;
            case Object::LargeBlob:
                type = "LargeBlob";
                break;
	    case Object::Purged:
		type = "Purged";
		break;
	    default:
		printf("Unknown object type!\n");
		assert(false);
	}
	printf("%s # %s\n", (*it).c_str(), type);
    }

    return 0;
}

int
cmd_purgeobj(int argc, const char *argv[])
{
    if (argc != 2) {
	cout << "Error: Incorrect number of arguements." << endl;
	cout << "ori purgeobj <OBJID>" << endl;
	return 1;
    }

    if (repository.getObjectType(argv[1]) != Object::Blob) {
	cout << "Error: You can only purge an object with type Blob." << endl;
	return 1;
    }

    if (!repository.purgeObject(argv[1])) {
	cout << "Error: Failed to purge object." << endl;
	return 1;
    }

    return 0;
}

int
commitHelper(void *arg, const char *path)
{
    string filePath = path;
    string hash;
    Tree *tree = (Tree *)arg;

    if (Util_IsDirectory(path)) {
	Tree subTree = Tree();

	Scan_Traverse(path, &subTree, commitHelper);

	hash = repository.addTree(subTree);
    } else {
	hash = repository.addFile(path);
    }
    tree->addObject(filePath.c_str(), hash);

    return 0;
}

void
usage_commit(void)
{
    cout << "ori commit [MESSAGE]" << endl;
    cout << endl;
    cout << "Commit any outstanding changes into the repository." << endl;
    cout << endl;
    cout << "An optional message can be added to the commit." << endl;
}

int
cmd_commit(int argc, const char *argv[])
{
    string blob;
    string treeHash, commitHash;
    string msg;
    string user;
    Tree tree = Tree();
    Commit commit = Commit();
    string root = Repo::findRootPath();

    if (argc == 1) {
        msg = "No message.";
    } else if (argc == 2) {
        msg = argv[1];
    }

    Scan_Traverse(root.c_str(), &tree, commitHelper);

    treeHash = repository.addTree(tree);

    // XXX: Get parents
    commit.setTree(treeHash);
    commit.setParents(repository.getHead());
    commit.setMessage(msg);
    commit.setTime(time(NULL));

    user = Util_GetFullname();
    if (user != "")
        commit.setUser(user);

    commitHash = repository.addCommit(commit);

    // Update .ori/HEAD
    repository.updateHead(commitHash);

    printf("Commit Hash: %s\nTree Hash: %s\n%s",
	   commitHash.c_str(),
	   treeHash.c_str(),
	   blob.c_str());

    return 0;
}

int
StatusDirectoryCB(void *arg, const char *path)
{
    string repoRoot = Repo::findRootPath();
    string objPath = path;
    string objHash;
    map<string, string> *dirState = (map<string, string> *)arg;

    if (!Util_IsDirectory(objPath)) {
	objHash = Util_HashFile(objPath);
        objPath = objPath.substr(repoRoot.size());

	dirState->insert(pair<string, string>(objPath, objHash));
    } else {
	objPath = objPath.substr(repoRoot.size());
	dirState->insert(pair<string, string>(objPath, "DIR"));
    }

    return 0;
}

int
StatusTreeIter(map<string, string> *tipState,
	       const string &path,
	       const string &treeId)
{
    Tree tree;
    map<string, TreeEntry>::iterator it;

    // XXX: Error handling
    tree = repository.getTree(treeId);

    for (it = tree.tree.begin(); it != tree.tree.end(); it++) {
	if ((*it).second.type == TreeEntry::Tree) {
	    tipState->insert(pair<string, string>(path + "/" + (*it).first,
						  "DIR"));
	    StatusTreeIter(tipState,
			   path + "/" + (*it).first,
			   (*it).second.hash);
	} else {
	    tipState->insert(pair<string, string>(path + "/" + (*it).first,
				    (*it).second.hash));
	}
    }

    return 0;
}

int
cmd_status(int argc, const char *argv[])
{
    map<string, string> dirState;
    map<string, string> tipState;
    map<string, string>::iterator it;
    Commit c;
    string tip = repository.getHead();

    if (tip != EMPTY_COMMIT) {
        c = repository.getCommit(tip);
	StatusTreeIter(&tipState, "", c.getTree());
    }

    Scan_RTraverse(Repo::findRootPath().c_str(),
		   (void *)&dirState,
	           StatusDirectoryCB);

    for (it = dirState.begin(); it != dirState.end(); it++) {
	map<string, string>::iterator k = tipState.find((*it).first);
	if (k == tipState.end()) {
	    printf("A	%s\n", (*it).first.c_str());
	} else if ((*k).second != (*it).second) {
	    // XXX: Handle replace a file <-> directory with same name
	    printf("M	%s\n", (*it).first.c_str());
	}
    }

    for (it = tipState.begin(); it != tipState.end(); it++) {
	map<string, string>::iterator k = dirState.find((*it).first);
	if (k == dirState.end()) {
	    printf("D	%s\n", (*it).first.c_str());
	}
    }

    return 0;
}

int
cmd_checkout(int argc, const char *argv[])
{
    map<string, string> dirState;
    map<string, string> tipState;
    map<string, string>::iterator it;
    Commit c;
    string tip = repository.getHead();

    if (argc == 2) {
	tip = argv[1];
    }

    if (tip != EMPTY_COMMIT) {
        c = repository.getCommit(tip);
	StatusTreeIter(&tipState, "", c.getTree());
    }

    Scan_RTraverse(Repo::findRootPath().c_str(),
		   (void *)&dirState,
	           StatusDirectoryCB);

    for (it = dirState.begin(); it != dirState.end(); it++) {
	map<string, string>::iterator k = tipState.find((*it).first);
	if (k == tipState.end()) {
	    printf("A	%s\n", (*it).first.c_str());
	} else if ((*k).second != (*it).second) {
	    printf("M	%s\n", (*it).first.c_str());
	    // XXX: Handle replace a file <-> directory with same name
	    assert((*it).second != "DIR");
	    repository.copyObject((*k).second,
				  Repo::findRootPath()+(*k).first);
	}
    }

    for (it = tipState.begin(); it != tipState.end(); it++) {
	map<string, string>::iterator k = dirState.find((*it).first);
	if (k == dirState.end()) {
	    string path = Repo::findRootPath() + (*it).first;
	    if ((*it).second == "DIR") {
		printf("N	%s\n", (*it).first.c_str());
		mkdir(path.c_str(), 0755);
	    } else {
		printf("U	%s\n", (*it).first.c_str());
		if (repository.getObjectType((*it).second) != Object::Purged)
		    repository.copyObject((*it).second, path);
		else
		    cout << "Object has been purged." << endl;
	    }
	}
    }

    return 0;
}

int
cmd_log(int argc, const char *argv[])
{
    string commit = repository.getHead();

    while (commit != EMPTY_COMMIT) {
	Commit c = repository.getCommit(commit);
    time_t timeVal = c.getTime();
    char timeStr[26];

    ctime_r(&timeVal, timeStr);

	printf("commit:  %s\n", commit.c_str());
	printf("parents: %s\n", c.getParents().first.c_str());
    printf("date:    %s\n", timeStr);
	printf("%s\n\n", c.getMessage().c_str());

	commit = c.getParents().first;
	// XXX: Handle merge cases
    }

    return 0;
}

int
cmd_clone(int argc, const char *argv[])
{
    string srcRoot;
    string newRoot;
    const char *initArgs[2] = { NULL, NULL };

    if (argc != 2 && argc != 3) {
	printf("Specify a repository to clone.\n");
	printf("usage: ori clone <repo> [<dir>]\n");
	return 1;
    }

    srcRoot = argv[1];
    if (argc == 2) {
	newRoot = srcRoot.substr(srcRoot.rfind("/")+1);
    } else {
	newRoot = argv[2];
    }
    initArgs[1] = newRoot.c_str();
    cmd_init(2, initArgs);

    printf("Cloning from %s to %s\n", srcRoot.c_str(), newRoot.c_str());

    Repo srcRepo(srcRoot);
    Repo dstRepo(newRoot);

    dstRepo.pull(&srcRepo);

    // XXX: Need to rely on sync log.
    dstRepo.updateHead(srcRepo.getHead());

    return 0;
}

int
cmd_pull(int argc, const char *argv[])
{
    string srcRoot;

    if (argc != 2) {
	printf("Specify a repository to pull.\n");
	printf("usage: ori pull <repo>\n");
	return 1;
    }

    srcRoot = argv[1];

    std::auto_ptr<SshClient> client;
    std::auto_ptr<BasicRepo> srcRepo;
    if (Util_IsPathRemote(srcRoot.c_str())) {
        client.reset(new SshClient(srcRoot));
        srcRepo.reset(new SshRepo(client.get()));
        client->connect();
    }
    else {
        srcRepo.reset(new Repo(srcRoot));
    }

    printf("Pulling from %s\n", srcRoot.c_str());
    repository.pull(srcRepo.get());

    // XXX: Need to rely on sync log.
    repository.updateHead(srcRepo->getHead());

    return 0;
}

/*
 * Verify the repository.
 */
int
cmd_verify(int argc, const char *argv[])
{
    int status = 0;
    string error;
    set<string> objects = repository.listObjects();
    set<string>::iterator it;

    for (it = objects.begin(); it != objects.end(); it++)
    {
	error = repository.verifyObject(*it);
	if (error != "") {
	    printf("Object %s\n%s\n", (*it).c_str(), error.c_str());
	    status = 1;
	}
    }

    return status;
}

/*
 * Find lost Heads
 */
int
cmd_findheads(int argc, const char *argv[])
{
    map<string, set<string> > refs;
    map<string, set<string> >::iterator it;

    refs = repository.computeRefCounts();

    for (it = refs.begin(); it != refs.end(); it++) {
        if ((*it).first == EMPTY_COMMIT)
            continue;

        if ((*it).second.size() == 0
            && repository.getObjectType((*it).first)) {
            Commit c = repository.getCommit((*it).first);

            // XXX: Check for existing branch names

            cout << "commit:  " << (*it).first << endl;
            cout << "parents: " << c.getParents().first << endl;
            cout << c.getMessage() << endl;
        }
    }

    return 0;
}

/*
 * Reference Counting Operations
 */

/*
 * Rebuild the reference counts.
 */
int
cmd_rebuildrefs(int argc, const char *argv[])
{
    map<string, set<string> > refs;
    map<string, set<string> >::iterator it;

    if (argc != 1) {
        cout << "rebuildrefs takes no arguements!" << endl;
        cout << "Usage: ori rebuildrefs" << endl;
    }

    refs = repository.computeRefCounts();

    for (it = refs.begin(); it != refs.end(); it++) {
        int status;
        Object o = Object();
        Object::Type type;

        if ((*it).first == EMPTY_COMMIT)
            continue;

        status = o.open(repository.objIdToPath((*it).first));
        if (status < 0) {
            cout << "Cannot open object " << (*it).first << endl;
            return 1;
        }
        type = o.getType();

        if (type == Object::Commit ||
            type == Object::Tree ||
            type == Object::Blob) {
            set<string>::iterator i;

            o.clearMetadata(); // was clearBackref
            for (i = (*it).second.begin(); i != (*it).second.end(); i++) {
                o.addBackref((*i), Object::BRRef);
            }
        } else if (type == Object::Purged) {
            set<string>::iterator i;

            o.clearMetadata(); // was clearBackref
            for (i = (*it).second.begin(); i != (*it).second.end(); i++) {
                o.addBackref((*i), Object::BRPurged);
            }
        } else {

            NOT_IMPLEMENTED(false);
        }

        o.close();
    }

    return 0;
}

/*
 * Print the reference counts for all objects, or show the references
 * to a particular object.
 */
int
cmd_refcount(int argc, const char *argv[])
{
    if (argc == 1) {
        map<string, map<string, Object::BRState> > refs;
        map<string, map<string, Object::BRState> >::iterator it;

        refs = repository.getRefCounts();

        cout << left << setw(64) << "Object" << " Count" << endl;
        for (it = refs.begin(); it != refs.end(); it++) {
            cout << (*it).first << " " << (*it).second.size() << endl;
        }
    } else if (argc == 2) {
        map<string, Object::BRState> refs;
        map<string, Object::BRState>::iterator it;

        refs = repository.getRefs(argv[1]);

        for (it = refs.begin(); it != refs.end(); it++) {
            cout << (*it).first << endl;
        }
    } else {
        cout << "Invalid number of arguements." << endl;
        cout << "ori refcount [OBJID]" << endl;
    }

    return 0;
}

/*
 * Graft help
 */
void
usage_graft(void)
{
    cout << "ori graft <Source Path> <Destination Directory>" << endl;
    cout << endl;
    cout << "Graft a subtree from a repository." << endl;
    cout << endl;
}

/*
 * Graft a subtree into a new tree with a patched history.
 *
 * XXX: Eventually we need to implement a POSIX compliant copy command.
 */
int
cmd_graft(int argc, const char *argv[])
{
    string srcRoot, dstRoot, srcRelPath, dstRelPath;
    Repo srcRepo, dstRepo;

    if (argc != 3) {
        cout << "Error in correct number of arguments." << endl;
        cout << "ori graft <Source Path> <Destination Path>" << endl;
        return 1;
    }

    // Convert relative paths to full paths.
    srcRelPath = Util_RealPath(argv[1]);
    dstRelPath = Util_RealPath(argv[2]);

    if (srcRelPath == "" || dstRelPath == "") {
        cout << "Error: Unable to resolve relative paths." << endl;
        return 1;
    }

    srcRoot = Repo::findRootPath(srcRelPath);
    dstRoot = Repo::findRootPath(dstRelPath);

    if (srcRoot == "") {
        cout << "Error: source path is not a repository." << endl;
        return 1;
    }

    if (dstRoot == "") {
        cout << "Error: destination path is not a repository." << endl;
        return 1;
    }

    srcRepo.open(srcRoot);
    dstRepo.open(dstRoot);

    // Transform the paths to be relative to repository root.
    srcRelPath = srcRelPath.substr(srcRoot.length());
    dstRelPath = dstRelPath.substr(dstRoot.length());

    cout << srcRelPath << endl;
    cout << dstRelPath << endl;

    dstRepo.graftSubtree(&srcRepo, srcRelPath, dstRelPath);

    return 0;
}

