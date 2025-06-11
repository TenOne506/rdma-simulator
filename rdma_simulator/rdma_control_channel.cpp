#include "rdma_control_channel.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <iostream>

RdmaControlChannel::RdmaControlChannel()
    : server_socket_(-1), client_socket_(-1), running_(false) {
}

RdmaControlChannel::~RdmaControlChannel() {
    cleanup();
}

bool RdmaControlChannel::start_server(uint16_t port) {
    server_socket_ = socket(AF_INET, SOCK_STREAM, 0);
    if (server_socket_ < 0) {
        handle_error("Failed to create server socket");
        return false;
    }

    // 设置socket选项
    int opt = 1;
    if (setsockopt(server_socket_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        handle_error("Failed to set socket options");
        ::close(server_socket_);
        server_socket_ = -1;
        return false;
    }

    // 绑定地址
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);

    if (bind(server_socket_, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        handle_error("Failed to bind server socket");
        ::close(server_socket_);
        server_socket_ = -1;
        return false;
    }

    // 开始监听
    if (listen(server_socket_, 1) < 0) {
        handle_error("Failed to listen on server socket");
        ::close(server_socket_);
        server_socket_ = -1;
        return false;
    }

    std::cout << "Server started on port " << port << std::endl;
    return true;
}

bool RdmaControlChannel::wait_for_client(uint32_t timeout_ms) {
    if (server_socket_ < 0) {
        handle_error("Server not started");
        return false;
    }

    // 设置非阻塞模式
    int flags = fcntl(server_socket_, F_GETFL, 0);
    fcntl(server_socket_, F_SETFL, flags | O_NONBLOCK);

    // 等待客户端连接
    auto start_time = std::chrono::steady_clock::now();
    while (true) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        client_socket_ = accept(server_socket_, (struct sockaddr*)&client_addr, &client_len);

        if (client_socket_ >= 0) {
            std::cout << "Client connected from " << inet_ntoa(client_addr.sin_addr) << std::endl;
            running_ = true;
            receive_thread_ = std::thread(&RdmaControlChannel::receive_loop, this);
            return true;
        }

        if (errno != EAGAIN && errno != EWOULDBLOCK) {
            handle_error("Failed to accept client connection");
            return false;
        }

        auto current_time = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            current_time - start_time).count();
        if (elapsed >= timeout_ms) {
            handle_error("Timeout waiting for client connection");
            return false;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

bool RdmaControlChannel::connect_to_server(const std::string& server_ip, uint16_t port, uint32_t timeout_ms) {
    client_socket_ = socket(AF_INET, SOCK_STREAM, 0);
    if (client_socket_ < 0) {
        handle_error("Failed to create client socket");
        return false;
    }

    // 设置非阻塞模式
    int flags = fcntl(client_socket_, F_GETFL, 0);
    fcntl(client_socket_, F_SETFL, flags | O_NONBLOCK);

    // 连接服务器
    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);
    inet_pton(AF_INET, server_ip.c_str(), &server_addr.sin_addr);

    int ret = connect(client_socket_, (struct sockaddr*)&server_addr, sizeof(server_addr));
    if (ret < 0 && errno != EINPROGRESS) {
        handle_error("Failed to connect to server");
        ::close(client_socket_);
        client_socket_ = -1;
        return false;
    }

    // 等待连接完成
    auto start_time = std::chrono::steady_clock::now();
    while (true) {
        fd_set write_fds;
        FD_ZERO(&write_fds);
        FD_SET(client_socket_, &write_fds);

        struct timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = 100000; // 100ms

        ret = select(client_socket_ + 1, nullptr, &write_fds, nullptr, &tv);
        if (ret > 0) {
            int error = 0;
            socklen_t len = sizeof(error);
            if (getsockopt(client_socket_, SOL_SOCKET, SO_ERROR, &error, &len) < 0 || error != 0) {
                handle_error("Connection failed");
                ::close(client_socket_);
                client_socket_ = -1;
                return false;
            }
            break;
        }

        auto current_time = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            current_time - start_time).count();
        if (elapsed >= timeout_ms) {
            handle_error("Connection timeout");
            ::close(client_socket_);
            client_socket_ = -1;
            return false;
        }
    }

    std::cout << "Connected to server " << server_ip << ":" << port << std::endl;
    running_ = true;
    receive_thread_ = std::thread(&RdmaControlChannel::receive_loop, this);
    return true;
}

bool RdmaControlChannel::send_connect_request(const RdmaQPInfo& local_qp_info) {
    RdmaControlMsg msg;
    msg.header.type = RdmaControlMsgType::CONNECT_REQ;
    msg.header.length = sizeof(RdmaQPInfo);
    msg.header.seq_num = 0;
    msg.payload.resize(sizeof(RdmaQPInfo));
    memcpy(msg.payload.data(), &local_qp_info, sizeof(RdmaQPInfo));
    return send_message(msg);
}

bool RdmaControlChannel::send_connect_response(const RdmaQPInfo& local_qp_info, bool accept) {
    RdmaControlMsg msg;
    msg.header.type = RdmaControlMsgType::CONNECT_RESP;
    msg.header.length = sizeof(RdmaQPInfo) + sizeof(bool);
    msg.header.seq_num = 0;
    msg.payload.resize(sizeof(RdmaQPInfo) + sizeof(bool));
    memcpy(msg.payload.data(), &local_qp_info, sizeof(RdmaQPInfo));
    memcpy(msg.payload.data() + sizeof(RdmaQPInfo), &accept, sizeof(bool));
    return send_message(msg);
}

bool RdmaControlChannel::send_qp_info(const RdmaQPInfo& qp_info) {
    RdmaControlMsg msg;
    msg.header.type = RdmaControlMsgType::QP_INFO;
    msg.header.length = sizeof(RdmaQPInfo);
    msg.header.seq_num = 0;
    msg.payload.resize(sizeof(RdmaQPInfo));
    memcpy(msg.payload.data(), &qp_info, sizeof(RdmaQPInfo));
    return send_message(msg);
}

bool RdmaControlChannel::send_mr_info(const RdmaMRInfo& mr_info) {
    RdmaControlMsg msg;
    msg.header.type = RdmaControlMsgType::MR_INFO;
    msg.header.length = sizeof(RdmaMRInfo);
    msg.header.seq_num = 0;
    msg.payload.resize(sizeof(RdmaMRInfo));
    memcpy(msg.payload.data(), &mr_info, sizeof(RdmaMRInfo));
    return send_message(msg);
}

bool RdmaControlChannel::send_ready() {
    RdmaControlMsg msg;
    msg.header.type = RdmaControlMsgType::READY;
    msg.header.length = 0;
    msg.header.seq_num = 0;
    return send_message(msg);
}

bool RdmaControlChannel::send_error(const std::string& error_msg) {
    RdmaControlMsg msg;
    msg.header.type = RdmaControlMsgType::ERROR;
    msg.header.length = error_msg.length();
    msg.header.seq_num = 0;
    msg.payload.resize(error_msg.length());
    memcpy(msg.payload.data(), error_msg.c_str(), error_msg.length());
    return send_message(msg);
}

bool RdmaControlChannel::receive_message(RdmaControlMsg& msg, uint32_t timeout_ms) {
    std::unique_lock<std::mutex> lock(queue_mutex_);
    if (message_queue_.empty()) {
        if (timeout_ms == 0) {
            return false;
        }
        if (!queue_cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms),
            [this] { return !message_queue_.empty() || !running_; })) {
            return false;
        }
    }
    
    if (!running_) {
        return false;
    }
    
    msg = message_queue_.front();
    message_queue_.pop();
    return true;
}

void RdmaControlChannel::register_message_callback(MessageCallback callback) {
    std::lock_guard<std::mutex> lock(callback_mutex_);
    message_callback_ = callback;
}

void RdmaControlChannel::register_error_callback(ErrorCallback callback) {
    std::lock_guard<std::mutex> lock(callback_mutex_);
    error_callback_ = callback;
}

bool RdmaControlChannel::is_connected() const {
    return running_ && (client_socket_ >= 0);
}

void RdmaControlChannel::close() {
    cleanup();
}

void RdmaControlChannel::receive_loop() {
    while (running_) {
        RdmaControlMsgHeader header;
        ssize_t ret = recv(client_socket_, &header, sizeof(header), MSG_WAITALL);
        if (ret <= 0) {
            if (ret == 0) {
                handle_error("Connection closed by peer");
            } else {
                handle_error("Failed to receive message header");
            }
            break;
        }

        RdmaControlMsg msg;
        msg.header = header;
        msg.payload.resize(header.length);

        if (header.length > 0) {
            ret = recv(client_socket_, msg.payload.data(), header.length, MSG_WAITALL);
            if (ret <= 0) {
                handle_error("Failed to receive message payload");
                break;
            }
        }

        {
            std::lock_guard<std::mutex> lock(queue_mutex_);
            message_queue_.push(msg);
            queue_cv_.notify_one();
        }

        {
            std::lock_guard<std::mutex> lock(callback_mutex_);
            if (message_callback_) {
                message_callback_(msg);
            }
        }
    }
}

bool RdmaControlChannel::send_message(const RdmaControlMsg& msg) {
    if (!is_connected()) {
        handle_error("Not connected");
        return false;
    }

    std::lock_guard<std::mutex> lock(socket_mutex_);
    
    // 发送消息头
    ssize_t ret = send(client_socket_, &msg.header, sizeof(msg.header), 0);
    if (ret <= 0) {
        handle_error("Failed to send message header");
        return false;
    }

    // 发送消息负载
    if (msg.header.length > 0) {
        ret = send(client_socket_, msg.payload.data(), msg.payload.size(), 0);
        if (ret <= 0) {
            handle_error("Failed to send message payload");
            return false;
        }
    }

    return true;
}

void RdmaControlChannel::handle_error(const std::string& error) {
    std::cerr << "Error: " << error << std::endl;
    {
        std::lock_guard<std::mutex> lock(callback_mutex_);
        if (error_callback_) {
            error_callback_(error);
        }
    }
}

void RdmaControlChannel::cleanup() {
    running_ = false;
    
    if (receive_thread_.joinable()) {
        receive_thread_.join();
    }

    if (client_socket_ >= 0) {
        ::close(client_socket_);
        client_socket_ = -1;
    }

    if (server_socket_ >= 0) {
        ::close(server_socket_);
        server_socket_ = -1;
    }

    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        while (!message_queue_.empty()) {
            message_queue_.pop();
        }
        queue_cv_.notify_all();
    }
} 