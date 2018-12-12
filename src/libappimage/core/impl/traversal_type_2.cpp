// system
#include <cstring>
#include <iostream>
#include <fstream>
#include <set>
#include <fcntl.h>

extern "C" {
#include <unistd.h>
#include <sys/stat.h>

// libraries
#include <nonstd.h>
#include <squashfuse.h>
#include <squashfs_fs.h>
}

// local
#include "utils/filesystem.h"
#include "core/appimage.h"
#include "core/exceptions.h"
#include "core/file_istream.h"
#include "streambuf_type_2.h"
#include "traversal_type_2.h"

using namespace std;
using namespace appimage::core::impl;

traversal_type_2::traversal_type_2(std::string path) : path(path) {
    clog << "Opening " << path << " as Type 2 AppImage" << endl;
    // read the offset at which a squashfs image is expected to start
    ssize_t fs_offset = core::appimage::getElfSize(path.c_str());

    if (fs_offset < 0)
        throw AppImageReadError("get_elf_size error");

    sqfs_err err = sqfs_open_image(&fs, path.c_str(), (size_t) fs_offset);

    if (err != SQFS_OK)
        throw AppImageReadError("sqfs_open_image error: " + path);

    // prepare for traverse
    rootInodeId = sqfs_inode_root(&fs);
    err = sqfs_traverse_open(&trv, &fs, rootInodeId);
    if (err != SQFS_OK)
        throw AppImageReadError("sqfs_traverse_open error");

}

traversal_type_2::~traversal_type_2() {
    sqfs_traverse_close(&trv);

    clog << "Closing " << path << " as Type 2 AppImage" << endl;
    sqfs_destroy(&fs);
}

void traversal_type_2::next() {
    sqfs_err err;
    if (!sqfs_traverse_next(&trv, &err))
        completed = true;

    if (err != SQFS_OK)
        throw AppImageReadError("sqfs_traverse_next error");
}

bool traversal_type_2::isCompleted() {
    return completed;
}

std::string traversal_type_2::getEntryName() {
    if (trv.path != nullptr)
        return trv.path;
    else
        return string();
}

void traversal_type_2::extract(const std::string& target) {
    sqfs_inode inode;
    if (sqfs_inode_get(&fs, &inode, trv.entry.inode))
        throw AppImageReadError("sqfs_inode_get error");

    // create target parent dir
    auto parentPath = utils::filesystem::parentPath(target);
    utils::filesystem::createDirectories(parentPath);

    // handle each inode type properly
    switch (inode.base.inode_type) {
        case SQUASHFS_DIR_TYPE:
        case SQUASHFS_LDIR_TYPE:
            extractDir(target);
            break;
        case SQUASHFS_REG_TYPE:
        case SQUASHFS_LREG_TYPE:
            extractFile(inode, target);
            break;
        case SQUASHFS_SYMLINK_TYPE:
        case SQUASHFS_LSYMLINK_TYPE:
            extractSymlink(inode, target);
            break;
        default:
            throw AppImageError("AppImage Type 2 inode.base.inode_type " +
                                std::to_string(inode.base.inode_type) + " not supported yet.");
    }
}

sqfs_err traversal_type_2::sqfs_stat(sqfs* fs, sqfs_inode* inode, struct stat* st) {
    // code borrowed from the "extract.c" file at squashfuse
    sqfs_err err;
    uid_t id;

    memset(st, 0, sizeof(*st));
    // fill stats
    st->st_mode = inode->base.mode;
    st->st_nlink = inode->nlink;
    st->st_mtime = st->st_ctime = st->st_atime = inode->base.mtime;

    if (S_ISREG(st->st_mode)) {
        /* FIXME: do symlinks, dirs, etc have a size? */
        st->st_size = inode->xtra.reg.file_size;
        st->st_blocks = st->st_size / 512;
    } else if (S_ISBLK(st->st_mode) || S_ISCHR(st->st_mode)) {
        st->st_rdev = sqfs_makedev(inode->xtra.dev.major,
                                   inode->xtra.dev.minor);
    } else if (S_ISLNK(st->st_mode)) {
        st->st_size = inode->xtra.symlink_size;
    }

    st->st_blksize = fs->sb.block_size; /* seriously? */

    err = sqfs_id_get(fs, inode->base.uid, &id);
    if (err)
        return err;
    st->st_uid = id;
    err = sqfs_id_get(fs, inode->base.guid, &id);
    st->st_gid = id;
    if (err)
        return err;

    return SQFS_OK;
}

void traversal_type_2::extractDir(const std::string& target) {
    if (access(target.c_str(), F_OK) == -1) { // The directory doesn't exists
        if (mkdir(target.c_str(), 0755) == -1) // Create new directory with 755 permissions
            throw AppImageError("mkdir error at " + target);
    }
}

void traversal_type_2::extractFile(sqfs_inode inode, const std::string& target) {
    // Read inode stats
    struct stat st = {};
    if (sqfs_stat(&fs, &inode, &st) != 0)
        throw AppImageReadError("sqfs_stat error");

    // open read stream
    auto& istream = read();

    // open write stream
    ofstream targetFile(target);

    // trasnfer data
    targetFile << istream.rdbuf();
    targetFile.close();

    // set file stats
    chmod(target.c_str(), st.st_mode);
}

void traversal_type_2::extractSymlink(sqfs_inode inode, const std::string& target) {
    // read the target link path size
    size_t size;
    sqfs_readlink(&fs, &inode, nullptr, &size);

    char buf[size];

    // read the target link in buf
    int ret = sqfs_readlink(&fs, &inode, buf, &size);
    if (ret != 0)
        throw AppImageReadError("sqfs_readlink error");

    // remove any existent link at t
    unlink(target.c_str());

    ret = symlink(buf, target.c_str());
    if (ret != 0)
        throw AppImageReadError("symlink error at " + target);
}

istream& traversal_type_2::read() {
    // get current inode
    sqfs_inode inode;
    if (sqfs_inode_get(&fs, &inode, trv.entry.inode))
        throw AppImageReadError("sqfs_inode_get error");

    // resolve symlinks if any
    if (!resolve_symlink(&inode))
        throw AppImageReadError("symlink resolution error");

    // create a streambuf for reading the inode contents
    auto streamBuffer = shared_ptr<streambuf>(new streambuf_type_2(fs, inode, 1024));
    appImageIStream.reset(new file_istream(streamBuffer));

    return *appImageIStream.get();
}

bool traversal_type_2::resolve_symlink(sqfs_inode* inode) {
    sqfs_err err;
    bool found = false;

    sqfs_inode rootInode;
    err = sqfs_inode_get(&fs, &rootInode, rootInodeId);
    if (err != SQFS_OK)
        return false;

    // Save visited inode numbers to prevent loops
    std::set<__le32> inodes_visited;
    inodes_visited.insert(inode->base.inode_number);

    while (inode->base.inode_type == SQUASHFS_SYMLINK_TYPE || inode->base.inode_type == SQUASHFS_LSYMLINK_TYPE) {
        // Read symlink
        size_t size;
        // read twice, once to find out right amount of memory to allocate
        err = sqfs_readlink(&fs, inode, NULL, &size);
        if (err != SQFS_OK)
            return false;

        char symlink_target_path[size];
        // then to populate the buffer
        err = sqfs_readlink(&fs, inode, symlink_target_path, &size);
        if (err != SQFS_OK)
            return false;

        // lookup symlink target path
        *inode = rootInode;
        err = sqfs_lookup_path(&fs, inode, symlink_target_path, &found);

        if (!found)
            return false;

        if (err != SQFS_OK)
            return false;

        // check if we fell into a loop
        if (inodes_visited.find(inode->base.inode_number) != inodes_visited.end()) {
            std::clog << "Symlinks loop found ";
            return false;
        } else
            inodes_visited.insert(inode->base.inode_number);
    }

    return true;
}
