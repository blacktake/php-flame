#pragma once
#include "controller.h"
#include "coroutine.h"

namespace flame {
    class command {
    public:
        command(coroutine_handler &ch)
        : ch_(ch) {}
        // 1. boost::asio::post(gcontroller->context_y, .....);
        // 2. ch_.suspend();
        virtual void execute() = 0;

        virtual void finish() {
            boost::asio::post(gcontroller->context_x, std::bind(&coroutine_handler::resume, ch_));
        }
    protected:
        coroutine_handler& ch_;
    };
}
