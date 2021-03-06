#pragma once
#include "../vendor.h"
#include "../coroutine.h"

namespace flame::mysql
{

    class _connection_base
    {
    public:
        // 此函数仅允许 escape 主线程调用
        static void escape(std::shared_ptr<MYSQL> c, php::buffer &b, const php::value &v, char quote = '\''); // 方便使用
        virtual std::shared_ptr<MYSQL> acquire(coroutine_handler& ch) = 0;
        php::object query(std::shared_ptr<MYSQL> conn, std::string sql, coroutine_handler& ch);
        php::array fetch(std::shared_ptr<MYSQL> conn, std::shared_ptr<MYSQL_RES> rst, MYSQL_FIELD *f, unsigned int n, coroutine_handler &ch);

      protected:
    };
} // namespace flame::mysql
