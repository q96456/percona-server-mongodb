/*
 * @Author: your name
 * @Date: 2020-11-11 15:47:11
 * @LastEditTime: 2020-11-16 14:43:24
 * @LastEditors: Please set LastEditors
 * @Description: In User Settings Edit
 * @FilePath: /percona-server-mongodb/src/mongo/db/repl/disk_check.cpp
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kSharding
#ifndef _WIN32
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif
#include "mongo/db/repl/disk_check.h"
#include "mongo/util/log.h"
#include "mongo/util/timer.h"
namespace mongo {
namespace repl {
DiskChecker::DiskChecker(){}
   

DiskChecker::~DiskChecker(){
     if (close(_fd)) {
        auto err = errno;
        //fassertNoTrace(4084, err == 0);
    }
    
}
bool DiskChecker::init(std::string db_path) {
     _db_path = db_path + "/checker";
    _fd = open(_db_path.c_str(), O_RDWR|O_CREAT|O_TRUNC, S_IWUSR);
    if (_fd == -1) {
        auto err = errno;
        //fassertNoTrace(4080, err == 0);
        return false;
    }

    return true;
}

void DiskChecker::checkDisk() {
    lseek(_fd,0,SEEK_SET);//将源文件的读写指针移到起始位置
    char write_info = 'a';
    Timer timer;
    while (true) {
        ssize_t bytesWrittenInWrite = write(_fd, &write_info, 1);
        if (bytesWrittenInWrite == -1) {
            //auto err = errno;
            if (err == EINTR) {
                continue;
            }

            log()<<"disk check write err="<<err;

            // LOGV2_FATAL_CONTINUE(
            //     23425,
            //     "write failed for '{file_generic_string}' with error: {errnoWithDescription_err}",
            //     "file_generic_string"_attr = file.generic_string(),
            //     "errnoWithDescription_err"_attr = errnoWithDescription(err));
            // fassertNoTrace(4081, err == 0);
        }

        if (fsync(_fd)) {
            //auto err = errno;
            log()<<"disk check  fsync err="<<err;
            // LOGV2_FATAL_CONTINUE(
            //     23426,
            //     "fsync failed for '{file_generic_string}' with error: {errnoWithDescription_err}",
            //     "file_generic_string"_attr = file.generic_string(),
            //     "errnoWithDescription_err"_attr = errnoWithDescription(err));
            // fassertNoTrace(4082, err == 0);
        }

        if (timer.millis() > 100){
            log()<<"check disk optime = "<<timer.millis()<<"ms";
        }

        return;
    }
}

}  // namespace repl
}  // namespace mongo