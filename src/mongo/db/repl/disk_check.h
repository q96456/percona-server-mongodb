/*
 * @Author: your name
 * @Date: 2020-11-11 15:47:09
 * @LastEditTime: 2020-11-11 19:34:18
 * @LastEditors: Please set LastEditors
 * @Description: In User Settings Edit
 * @FilePath: /percona-server-mongodb/src/mongo/db/repl/disk_check.h
 */
#pragma once
#include <boost/filesystem/path.hpp>
#include <functional>
#include <string>
#include <vector>
namespace mongo {
namespace repl {
class DiskChecker{
    public:
        DiskChecker();
        ~DiskChecker();
        bool init(std::string db_path);
        void checkDisk();
    private:
        std::string _db_path;
        int _fd;
};
}
}