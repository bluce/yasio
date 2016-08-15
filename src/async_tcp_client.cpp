//////////////////////////////////////////////////////////////////////////////////////////
// A cross platform socket APIs, support ios & android & wp8 & window store universal app
// version: 2.2
//////////////////////////////////////////////////////////////////////////////////////////
/*
The MIT License (MIT)

Copyright (c) 2012-2016 halx99

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
*/

//#include "oslib.h"
#include "async_tcp_client.h"

#ifdef _WIN32
#define msleep(msec) Sleep((msec))
#define sleep(sec) msleep((sec) * 1000)
#if !defined(WINRT)
#include <MMSystem.h>
#pragma comment(lib, "winmm.lib")
#endif
#else
#define msleep(msec) usleep((msec) * 1000)
#endif

#define MAX_WAIT_DURATION 5 * 60 * 1000 * 1000 // 5 minites

#define MAX_PDU_LEN SZ(10, M)

#define CALL_TSF(stmt) this->call_tsf_([=]{(stmt);});

namespace purelib {
namespace inet {
class appl_pdu
{
public:
    appl_pdu(std::vector<char>&& right, const appl_pdu_send_callback_t& callback, const std::chrono::microseconds& duration)
    {
        data_ = std::move(right);
        offset_ = 0;
        on_sent_ = callback;
        expire_time_ = std::chrono::steady_clock::now() + duration;
    }
    bool expired() const {
        return (expire_time_ - std::chrono::steady_clock::now()).count() < 0;
    }
    std::vector<char>          data_; // sending data
    size_t                     offset_; // offset
    appl_pdu_send_callback_t   on_sent_;
    compatible_timepoint_t     expire_time_;

    static void * operator new(size_t /*size*/)
    {
        return get_pool().get();
    }

    static void operator delete(void *p)
    {
        get_pool().release(p);
    }

    static purelib::gc::object_pool<appl_pdu>& get_pool()
    {
        static purelib::gc::object_pool<appl_pdu> s_pool;
        return s_pool;
    }
};

void p2p_io_ctx::reset()
{
    connected_ = false;
    receiving_pdu_.clear();
    offset_ = 0;
    expected_pdu_length_ = -1;
    error = 0;

    if (this->impl_.is_open()) {
        this->impl_.close();
    }
}

async_tcp_client::async_tcp_client() : app_exiting_(false),
    thread_started_(false),
    interrupter_(),
    address_("192.168.1.104"),
    port_(8001),
    connect_timeout_(3),
    send_timeout_(3),
    decode_pdu_length_(nullptr),
    error_(ErrorCode::ERR_OK),
    idle_(true),
    connect_failed_(false)
{
    FD_ZERO(&fdss_[read_op]);
    FD_ZERO(&fdss_[write_op]);
    FD_ZERO(&fdss_[read_op]);

    maxfdp_ = 0;
    reset();
}

async_tcp_client::~async_tcp_client()
{
    app_exiting_ = true;

    // shutdown_p2p_chancel();
    this->p2p_acceptor_.close();

    close();

    this->connect_notify_cv_.notify_all();
    while (working_counter_ > 0) { // wait all workers exited
        msleep(1);
    }
}

void async_tcp_client::set_timeouts(long timeo_connect, long timeo_send)
{
    this->connect_timeout_ = timeo_connect;
    this->send_timeout_ = timeo_send;
}

void async_tcp_client::set_endpoint(const char* address, const char* addressv6, u_short port)
{
    this->address_ = address;
    this->addressv6_ = address;
    this->port_ = port;
}

void async_tcp_client::set_callbacks(
    decode_pdu_length_func decode_length_func,
    build_error_func build_error_pdu_func,
    const appl_pdu_recv_callback_t& callback, 
    const std::function<void(const vdcallback_t&)>& threadsafe_call)
{
    this->decode_pdu_length_ = decode_length_func;
    this->on_received_pdu_ = callback;
    this->build_error_pdu_ = build_error_pdu_func;
    this->call_tsf_ = threadsafe_call;
}

size_t async_tcp_client::get_received_pdu_count(void) const
{
    return recv_queue_.size();
}

void async_tcp_client::dispatch_received_pdu(int count) {
    assert(this->on_received_pdu_ != nullptr);

    if (this->recv_queue_.empty())
        return;

    std::lock_guard<std::mutex> autolock(this->recv_queue_mtx_);
    do {
        auto packet = std::move(this->recv_queue_.front());
        this->recv_queue_.pop_front();
        this->on_received_pdu_(std::move(packet));
    } while (!this->recv_queue_.empty() && --count > 0);
}

void async_tcp_client::start_service(int working_counter)
{
    if (!thread_started_) {
        thread_started_ = true;
        //  this->scheduleCollectResponse();    //
        this->working_counter_ = working_counter;
        for (auto i = 0; i < working_counter; ++i) {
            std::thread t([this] {
                INET_LOG("async_tcp_client thread running...");
                this->service();
                --working_counter_;
                INET_LOG("async_tcp_client thread exited.");
            });
            t.detach();
        }
    }
}

void async_tcp_client::set_connect_listener(const connect_listener& listener)
{
    this->connect_listener_ = listener;
}

void async_tcp_client::wait_connect_notify()
{
    std::unique_lock<std::mutex> autolock(connect_notify_mtx_);
    connect_notify_cv_.wait(autolock); // wait 1 minutes, if no notify, wakeup self.
}

void async_tcp_client::service()
{
#if defined(_WIN32) && !defined(WINRT)
    timeBeginPeriod(1);
#endif

    if (this->suspended_)
        return;

    while (!app_exiting_) 
    {
        bool connection_ok = impl_.is_open();

        if (!connection_ok)
        { // if no connect, wait connect notify.
            if (total_connect_times_ >= 0)
                wait_connect_notify();
            ++total_connect_times_;
            connection_ok = this->connect();
            if (!connection_ok) { /// connect failed, waiting connect notify
                // reconnClock = clock(); never use clock api on android platform
                continue;
            }
        }


        // event loop
        fd_set fdss[3];
        timeval timeout;

        for (; !app_exiting_;)
        {
            memcpy(&fdss, fdss_, sizeof(fdss_));

            if (this->offset_ > 0) { // @pitfall: If read buffer has data, needs proccess firstly
                if (!do_read(this))
                    goto _L_error;

                // @wait only 1 millisecond, Let write operation has opportunity to perform.
                get_wait_duration(timeout, 1000);
            }
            else {
                get_wait_duration(timeout, MAX_WAIT_DURATION);
            }

            int nfds = ::select(maxfdp_, &(fdss[read_op]), &(fdss[write_op]), &(fdss[except_op]), &timeout);
           
            if (nfds == -1)
            {
                INET_LOG("select failed, error code: %d\n", xxsocket::get_last_errno());
                continue;            // try select again
            }

            if (nfds == 0)
            {
                perform_timeout_timers();
                continue;
            }

            /*
            ** check and handle error sockets
            */
#if 0
            for (auto i = 0; i < excep_set.fd_count; ++i) {
                if (excepfds_.fd_array[i] == this->impl_)
                {
                    goto _L_error;
                }
            }
#else
            if (FD_ISSET(this->impl_.native_handle(), &(fdss[except_op])))
            { // exception occured
                goto _L_error;
            }
#endif

            /*
            ** check and handle readable sockets
            */
#if 0
            for (auto i = 0; i < read_set.fd_count; ++i) {
                const auto readfd = read_set.fd_array[i];
                if (readfd == this->p2p_acceptor_)
                { // process p2p connect request from peer
                    p2p_do_accept();
                }
                else if (readfd == this->impl_)
                {
                    if (!do_read(this))
                        goto _L_error;
                    idle_ = false;
                }
                else if (readfd == this->p2p_channel2_.impl_) {
                    if (!do_read(&this->p2p_channel2_))
                    { // read failed
                        this->p2p_channel2_.reset();
                    }
                }
                else if (readfd == this->p2p_channel1_.impl_) {
                    if (this->p2p_channel1_.connected_)
                    {
                        if (!do_read(&this->p2p_channel1_))
                        { // read failed
                            this->p2p_channel2_.reset();
                        }
                    }
                    else {
                        printf("P2P: The connection local --> peer established. \n");
                        this->p2p_channel1_.connected_ = true;
                    }
                }
            }
#else
            if (FD_ISSET(this->impl_.native_handle(), &(fdss[read_op])))
            { // can read socket data
                if (!do_read(this))
                    goto _L_error;
            }
#endif

            if (FD_ISSET(this->interrupter_.read_descriptor(), &(fdss[read_op]))) {
                // perform write operations
                if (do_write(this))
                { // TODO: check would block? for client, may be unnecessory.
                    if (this->send_queue_.empty()) {
                        interrupter_.reset();
                    }
                }
                else {
                    goto _L_error;
                } 
            }

            perform_timeout_timers();

            if (this->p2p_channel1_.connected_) {
                if (!do_write(&p2p_channel1_))
                    p2p_channel1_.reset();
            }

            if (this->p2p_channel1_.connected_) {
                if (!do_write(&p2p_channel2_))
                    p2p_channel2_.reset();
            }
        }

        continue;

    _L_error:
        handle_error();
    }

#if defined(_WIN32) && !defined(WINRT)
    timeEndPeriod(1);
#endif
}

void async_tcp_client::close()
{
    if (impl_.is_open())
        impl_.close();
}

void async_tcp_client::notify_connect()
{
    std::unique_lock<std::mutex> autolock(connect_notify_mtx_);
    connect_notify_cv_.notify_one();
}

void async_tcp_client::handle_error(void)
{
    socket_error_ = xxsocket::get_last_errno();
    const char* errs = xxsocket::get_error_msg(socket_error_);

    if (impl_.is_open()) {
        impl_.close();
    }
    else {
        INET_LOG("local close the connection!");
    }

    if (this->build_error_pdu_) {
        this->receiving_pdu_ = build_error_pdu_(socket_error_, errs);;
        move_received_pdu(this);
    }

    std::lock_guard<std::recursive_mutex> lk(this->send_queue_mtx_);
    if (!send_queue_.empty()) {
        for (auto pdu : send_queue_)
            delete pdu;
        send_queue_.clear();
    }

    // @Notify all timers are cancelled.
    for (auto& timer : timer_queue_)
        timer->callback_(true); 
    this->timer_queue_.clear();
}

void async_tcp_client::register_descriptor(const socket_native_type fd, int flags)
{
    if ((flags & socket_event_read) != 0)
    {
        FD_SET(fd, &(fdss_[read_op]));
    }

    if ((flags & socket_event_write) != 0)
    {
        FD_SET(fd, &(fdss_[write_op]));
    }

    if ((flags & socket_event_except) != 0)
    {
        FD_SET(fd, &(fdss_[except_op]));
    }

    maxfdp_ = static_cast<int>(fd) + 1;
}

void async_tcp_client::unregister_descriptor(const socket_native_type fd, int flags)
{
    if ((flags & socket_event_read) != 0)
    {
        FD_CLR(fd, &(fdss_[read_op]));
    }

    if ((flags & socket_event_write) != 0)
    {
        FD_CLR(fd, &(fdss_[write_op]));
    }

    if ((flags & socket_event_except) != 0)
    {
        FD_CLR(fd, &(fdss_[except_op]));
    }
}

void async_tcp_client::async_send(std::vector<char>&& data, const appl_pdu_send_callback_t& callback)
{
    if (this->impl_.is_open())
    {
        auto pdu = new appl_pdu(std::move(data), callback, std::chrono::seconds(this->send_timeout_));
        
        std::unique_lock<std::recursive_mutex> autolock(send_queue_mtx_);
        send_queue_.push_back(pdu);

        interrupter_.interrupt();
    }
    else {
        INET_LOG("async_tcp_client::send failed, The connection not ok!!!");
    }
}

void async_tcp_client::move_received_pdu(p2p_io_ctx* ctx)
{
    INET_LOG("move_received_pdu...");

    recv_queue_mtx_.lock();
    recv_queue_.push_back(std::move(ctx->receiving_pdu_)); // no data copy
    recv_queue_mtx_.unlock();
    //auto pdu = ctx->receiving_pdu_; // make a copy
    //this->call_tsf_([pdu, this]() mutable -> void {
    //    this->on_received_pdu_(std::move(pdu));
    //});

    ctx->expected_pdu_length_ = -1;

    // CCSCHTASKS->resumeTarget(this);
}


bool async_tcp_client::connect(void)
{
    if (connect_failed_)
    {
        connect_failed_ = false;
    }

    INET_LOG("connecting server %s:%u...", address_.c_str(), port_);

    int ret = -1;
    this->connected_ = false;
    int flags = xxsocket::getipsv();
    if (flags & ipsv_ipv4) {
        ret = impl_.pconnect_n(this->address_.c_str(), this->port_, this->connect_timeout_);
    }
    else if (flags & ipsv_ipv6)
    { // client is IPV6_Only
        INET_LOG("Client needs a ipv6 server address to connect!");
        ret = impl_.pconnect_n(this->addressv6_.c_str(), this->port_, this->connect_timeout_);
    }
    if (ret == 0)
    { // connect succeed, reset fds
        FD_ZERO(&fdss_[read_op]);
        FD_ZERO(&fdss_[write_op]);
        FD_ZERO(&fdss_[except_op]);

        register_descriptor(interrupter_.read_descriptor(), socket_event_read);
        register_descriptor(impl_.native_handle(), socket_event_read | socket_event_except);

        impl_.set_nonblocking(true);

        expected_pdu_length_ = -1;
        error_ = ErrorCode::ERR_OK;
        offset_ = 0;
        receiving_pdu_.clear();
        recv_queue_.clear();

        this->impl_.set_optval(SOL_SOCKET, SO_REUSEADDR, 1); // set opt for p2p

        INET_LOG("connect server: %s:%u succeed.", address_.c_str(), port_);

#if 0 // provided as API
        auto endp = this->impl_.local_endpoint();
        // INETLOG("local endpoint: %s", endp.to_string().c_str());

        if (p2p_acceptor_.reopen())
        { // start p2p listener
            if (p2p_acceptor_.bind(endp) == 0)
            {
                if (p2p_acceptor_.listen() == 0)
                {
                    // set socket to async
                    p2p_acceptor_.set_nonblocking(true);

                    // register p2p's peer connect event
                    register_descriptor(p2p_acceptor_, socket_event_read);
                    // INETLOG("listening at local %s successfully.", endp.to_string().c_str());
                }
            }
        }
#endif

        this->connected_ = true;
        if (this->connect_listener_ != nullptr) {
            CALL_TSF(this->connect_listener_(true, 0));
        }

        return true;
    }
    else { // connect server failed.
        connect_failed_ = true;
        int ec = xxsocket::get_last_errno();
        if (this->connect_listener_ != nullptr) {
            CALL_TSF(this->connect_listener_(false, ec));
        }

        INET_LOG("connect server: %s:%u failed, error code:%d, error msg:%s!", address_.c_str(), port_, ec, xxsocket::get_error_msg(ec));

        return false;
    }
}

bool async_tcp_client::do_write(p2p_io_ctx* ctx)
{
    bool bRet = false;

    do {
        int n;

        if (!ctx->impl_.is_open())
            break;

        std::unique_lock<std::recursive_mutex> autolock(ctx->send_queue_mtx_);
        if (!ctx->send_queue_.empty()) {
            auto& v = ctx->send_queue_.front();
            auto bytes_left = v->data_.size() - v->offset_;
            n = ctx->impl_.send_i(v->data_.data() + v->offset_, bytes_left);
            if (n == bytes_left) { // All pdu bytes sent.
                ctx->send_queue_.pop_front();
                this->call_tsf_([v] {
                    if (v->on_sent_ != nullptr)
                        v->on_sent_(ErrorCode::ERR_OK);
                    delete v;
                });
            }
            else if (n > 0) { // TODO: add time
                if (!v->expired())
                { // change offset, remain data will send next time.
                    // v->data_.erase(v->data_.begin(), v->data_.begin() + n);
                    v->offset_ += n;
                }
                else { // send timeout
                    ctx->send_queue_.pop_front();
                    this->call_tsf_([v] {
                        if (v->on_sent_)
                            v->on_sent_(ErrorCode::ERR_SEND_TIMEOUT);
                        delete v;
                    });
                }
            }
            else { // n <= 0, TODO: add time
                socket_error_ = xxsocket::get_last_errno();
                if (SHOULD_CLOSE_1(n, socket_error_)) {
                    ctx->send_queue_.pop_front();
                    this->call_tsf_([v] {
                        if (v->on_sent_)
                            v->on_sent_(ErrorCode::ERR_CONNECTION_LOST);
                        delete v;
                    });

                    break;
                }
            }
            idle_ = false;
        }
        autolock.unlock();

        bRet = true;
    } while (false);

    return bRet;
}

bool async_tcp_client::do_read(p2p_io_ctx* ctx)
{
    bool bRet = false;
    do {
        if (!ctx->impl_.is_open())
            break;

        int n = ctx->impl_.recv_i(ctx->buffer_ + ctx->offset_, sizeof(buffer_) - ctx->offset_);

        if (n > 0 || (n == -1 && ctx->offset_ != 0)) {

            if (n == -1)
                n = 0;

            INET_LOG("async_tcp_client::doRead --- received data len: %d, buffer data len: %d", n, n + ctx->offset_);

            if (ctx->expected_pdu_length_ == -1) { // decode length
                if (decode_pdu_length_(ctx->buffer_, ctx->offset_ + n, ctx->expected_pdu_length_))
                {
                    if (ctx->expected_pdu_length_ < 0) { // header insuff
                        ctx->offset_ += n;
                    }
                    else { // ok
                        ctx->receiving_pdu_.reserve(ctx->expected_pdu_length_); // #perfomance, avoid memory reallocte.

                        auto bytes_transferred = n + ctx->offset_;
                        ctx->receiving_pdu_.insert(ctx->receiving_pdu_.end(), ctx->buffer_, ctx->buffer_ + (std::min)(ctx->expected_pdu_length_, bytes_transferred));
                        if (bytes_transferred >= expected_pdu_length_)
                        { // pdu received properly
                            ctx->offset_ = bytes_transferred - ctx->expected_pdu_length_; // set offset to bytes of remain buffer
                            if (offset_ > 0)  // move remain data to head of buffer and hold offset.
                                ::memmove(ctx->buffer_, ctx->buffer_ + ctx->expected_pdu_length_, offset_);

                            // move properly pdu to ready queue, GL thread will retrieve it.
                            move_received_pdu(ctx);
                        }
                        else { // all buffer consumed, set offset to ZERO, pdu incomplete, continue recv remain data.
                            ctx->offset_ = 0;
                        }
                    }
                }
                else {
                    error_ = ErrorCode::ERR_DPL_ILLEGAL_PDU;
                    break;
                }
            }
            else { // recv remain pdu data
                auto bytes_transferred = n + ctx->offset_;// bytes transferred at this time
                if ((receiving_pdu_.size() + bytes_transferred) > MAX_PDU_LEN) // TODO: config MAX_PDU_LEN, now is 16384
                {
                    error_ = ErrorCode::ERR_PDU_TOO_LONG;
                    break;
                }
                else {
                    auto bytes_needed = ctx->expected_pdu_length_ - static_cast<int>(ctx->receiving_pdu_.size()); // never equal zero or less than zero
                    if (bytes_needed > 0) {
                        ctx->receiving_pdu_.insert(ctx->receiving_pdu_.end(), ctx->buffer_, ctx->buffer_ + (std::min)(bytes_transferred, bytes_needed));

                        ctx->offset_ = bytes_transferred - bytes_needed; // set offset to bytes of remain buffer
                        if (ctx->offset_ >= 0)
                        { // pdu received properly
                            if (offset_ > 0)  // move remain data to head of buffer and hold offset.
                                ::memmove(ctx->buffer_, ctx->buffer_ + bytes_needed, ctx->offset_);

                            // move properly pdu to ready queue, GL thread will retrieve it.
                            move_received_pdu(ctx);
                        }
                        else { // all buffer consumed, set offset to ZERO, pdu incomplete, continue recv remain data.
                            ctx->offset_ = 0;
                        }
                    }
                    else {
                        //assert(false);
                    }
                }
            }

            this->idle_ = false;
        }
        else {
            socket_error_ = xxsocket::get_last_errno();
            if (SHOULD_CLOSE_0(n, socket_error_)) {
                if (n == 0) {
                    INET_LOG("server close the connection!");
                }
                break;
            }
        }

        bRet = true;

    } while (false);

    return bRet;
}

void async_tcp_client::schedule_timer(deadline_timer* timer)
{ 
    std::lock_guard<std::recursive_mutex> lk(this->timer_queue__mtx_);
    if (std::find(timer_queue_.begin(), timer_queue_.end(), timer) != timer_queue_.end())
        return;

    this->timer_queue_.push_back(timer);
    
    std::sort(this->timer_queue_.begin(), this->timer_queue_.end(), [](deadline_timer* lhs, deadline_timer* rhs) {
        return lhs->wait_duration() > rhs->wait_duration();
    });

    interrupter_.interrupt();
}

void async_tcp_client::cancel_timer(deadline_timer* timer)
{
    std::lock_guard<std::recursive_mutex> lk(this->timer_queue__mtx_);

    auto iter = std::find(timer_queue_.begin(), timer_queue_.end(), timer);
    if (iter != timer_queue_.end()) {
        timer->callback_(true);
        timer_queue_.erase(iter);
    }
}

void async_tcp_client::perform_timeout_timers()
{
    std::lock_guard<std::recursive_mutex> lk(this->timer_queue__mtx_);

    std::vector<deadline_timer*> loop_timers;
    while (!this->timer_queue_.empty())
    {
        auto earliest = timer_queue_.back();
        if (earliest->expired())
        {
            timer_queue_.pop_back();
            earliest->callback_(false);
            if (earliest->loop_) {
                earliest->expires_from_now();
                loop_timers.push_back(earliest);
            }
        } 
        else {
            break;
        }
    }

    if (!loop_timers.empty()) {
        this->timer_queue_.insert(this->timer_queue_.end(), loop_timers.begin(), loop_timers.end());
        std::sort(this->timer_queue_.begin(), this->timer_queue_.end(), [](deadline_timer* lhs, deadline_timer* rhs) {
            return lhs->wait_duration() > rhs->wait_duration();
        });
    }
}

void  async_tcp_client::get_wait_duration(timeval& tv, long long usec)
{
    auto earliest = !this->timer_queue_.empty() ? timer_queue_.back() : nullptr;
    std::chrono::microseconds min_duration(usec); // microseconds
    if (earliest != nullptr) {
        auto duration = earliest->wait_duration();
        if (min_duration > duration)
            min_duration = duration;
    }

    usec = min_duration.count();

    tv.tv_sec = usec / 1000000;
    tv.tv_usec = usec % 1000000;
}

void async_tcp_client::p2p_open()
{
    if (is_connected()) {
        if (this->p2p_acceptor_.reopen())
        {
            this->p2p_acceptor_.bind(this->impl_.local_endpoint());
            this->p2p_acceptor_.listen(1); // We just listen one connection for p2p
            register_descriptor(this->p2p_acceptor_, socket_event_read);
        }
    }
}

bool async_tcp_client::p2p_do_accept(void)
{
    if (this->p2p_channel2_.impl_.is_open())
    { // Just ignore other connect request for 1 <<-->> 1 connections.
        this->p2p_acceptor_.accept().close();
        return true;
    }

    this->idle_ = false;
    this->p2p_channel2_.impl_ = this->p2p_acceptor_.accept();
    return this->p2p_channel2_.impl_.is_open();
}
#if 0
void async_tcp_client::dispatchResponseCallbacks(float delta)
{
    std::unique_lock<std::mutex> autolock(this->recvQueueMtx);
    if (!this->recvQueue.empty()) {
        auto pdu = std::move(this->recvQueue.front());
        this->recvQueue.pop();

        if (this->recvQueue.empty()) {
            autolock.unlock();
            //CCSCHTASKS->pauseTarget(this);
        }
        else
            autolock.unlock();

        if (onRecvpdu != nullptr)
            this->onRecvpdu(std::move(pdu));
    }
    else {
        autolock.unlock();
        //CCSCHTASKS->pauseTarget(this);
    }
}
#endif
} /* namespace purelib::net */
} /* namespace purelib */
