/*
 * @Author: your name
 * @Date: 2020-11-05 16:56:25
 * @LastEditTime: 2020-11-09 15:57:03
 * @LastEditors: Please set LastEditors
 * @Description: In User Settings Edit
 * @FilePath: /percona-server-mongodb/src/mongo/db/s/auto_refresh_routing.h
 */
#pragma once

#include "mongo/util/background.h"
#include "mongo/util/time_support.h"

namespace mongo {

class AutoRefreshRouting : public PeriodicTask {
public:
    explicit AutoRefreshRouting(uint64_t start);

    /**
     * Gets the PeriodicTask's name.
     * @return CertificateExpirationMonitor's name.
     */
    virtual std::string taskName() const;

    /**
     * Wakes up every minute as it is a PeriodicTask.
     * 每条secondary 刷一下路由信息
     */
    virtual void taskDoWork();

private:
    std::string _autoRefreshRoutingNameSpace;
    uint64_t _nextRefreshTime;

};


}  // namespace mongo