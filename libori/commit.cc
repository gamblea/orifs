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

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

#include <iostream>
#include <string>

#include "commit.h"
#include "util.h"
#include "stream.h"

using namespace std;

/********************************************************************
 *
 *
 * Commit
 *
 *
 ********************************************************************/

Commit::Commit()
{
    snapshotName = "";
    graftRepo = "";
    graftPath = "";
}

Commit::~Commit()
{
}

void
Commit::setParents(ObjectHash p1, ObjectHash p2)
{
    parents.first = p1;
    parents.second = p2;
}

pair<ObjectHash, ObjectHash>
Commit::getParents() const
{
    return parents;
}

void
Commit::setMessage(const string &msg)
{
    message = msg;
}

string
Commit::getMessage() const
{
    return message;
}

void
Commit::setTree(const ObjectHash &tree)
{
    treeObjId = tree;
}

ObjectHash
Commit::getTree() const
{
    return treeObjId;
}

void
Commit::setUser(const string &user)
{
    this->user = user;
}

string
Commit::getUser() const
{
    return user;
}

void
Commit::setSnapshot(const std::string &snapshot)
{
    snapshotName = snapshot;
}

std::string
Commit::getSnapshot() const
{
    return snapshotName;
}

void
Commit::setTime(time_t t)
{
    date = t;
}

time_t
Commit::getTime() const
{
    return date;
}

void
Commit::setGraft(const string &repo,
		 const string &path,
		 const ObjectHash &commitId)
{
    graftRepo = repo;
    graftPath = path;
    graftCommitId = commitId;
}

pair<string, string>
Commit::getGraftRepo() const
{
    return make_pair(graftRepo, graftPath);
}

ObjectHash
Commit::getGraftCommit() const
{
    return graftCommitId;
}

string
Commit::getBlob() const
{
    strwstream ss;

    ss.writeHash(treeObjId);
    if (!parents.second.isEmpty()) {
        ss.writeInt<uint8_t>(2);
        ss.writeHash(parents.first);
        ss.writeHash(parents.second);
    }
    else {
        ss.writeInt<uint8_t>(1);
        ss.writeHash(parents.first);
    }

    ss.writePStr(user);
    ss.writeInt(date);
    ss.writePStr(snapshotName);

#ifdef DEBUG
    if (graftRepo != "") {
	assert(graftPath != "");
	assert(!graftCommitId.isEmpty());
    }
#endif

    ss.writePStr(graftRepo);
    ss.writePStr(graftPath);
    ss.writeHash(graftCommitId);
    
    ss.writePStr(message);

    return ss.str();
}

void
Commit::fromBlob(const string &blob)
{
    strstream ss(blob);

    ss.readHash(treeObjId);
    uint8_t numParents = ss.readInt<uint8_t>();
    if (numParents == 2) {
        ss.readHash(parents.first);
        ss.readHash(parents.second);
    }
    else {
        ss.readHash(parents.first);
    }

    ss.readPStr(user);
    date = ss.readInt<time_t>();
    ss.readPStr(snapshotName);

    ss.readPStr(graftRepo);
    ss.readPStr(graftPath);
    ss.readHash(graftCommitId);
    
#ifdef DEBUG
    if (graftRepo != "") {
	assert(graftPath != "");
	assert(!graftCommitId.isEmpty());
    }
#endif

    ss.readPStr(message);

    // Verify that everything is set!
}

ObjectHash
Commit::hash() const
{
    return Util_HashString(getBlob());
}
