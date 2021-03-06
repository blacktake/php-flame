#pragma once
#include "../vendor.h"
#include "../coroutine.h"
#include "../url.h"
#include "_connection_base.h"


namespace flame::mysql
{
    class _connection_pool : public _connection_base, public std::enable_shared_from_this<_connection_pool>
    {
    public:
        _connection_pool(url u);
        ~_connection_pool();
        std::shared_ptr<MYSQL> acquire(coroutine_handler &ch) override;
        void sweep();
    private:
        url                               url_;
        const std::uint16_t               min_;
        const std::uint16_t               max_;
        std::uint16_t                    size_;

        boost::asio::io_context::strand guard_; // 防止对下面队列操作发生多线程问题;
        std::list<std::function<void(std::shared_ptr<MYSQL>)>> await_;
        struct connection_t
        {
            MYSQL *conn;
            std::chrono::time_point<std::chrono::steady_clock> ttl;
        };
        std::list<connection_t> conn_;
        boost::asio::steady_timer tm_;

        boost::logic::tribool reset_;
        boost::logic::tribool charset_;
        
        MYSQL* create();
        void release(MYSQL *c);
        void query_charset(MYSQL* c);
    };

} // namespace flame::mysql
