#ifndef AUTH_MIDDLEWARE_HPP
#define AUTH_MIDDLEWARE_HPP

#include "sessionmanager.hpp"
#include "crow_all.h"
#include <string>
#include <memory>

class AuthMiddleware {
public:
    struct context {
        bool is_admin;
        std::shared_ptr<SessionManager::Session> session;

        context() : is_admin(false), session(nullptr) {}
        context(bool admin, std::shared_ptr<SessionManager::Session> sess)
            : is_admin(admin), session(sess) {}
    };

    AuthMiddleware(const std::string& admin_password)
        : session_manager_(), admin_password_(admin_password) {}

    // 认证中间件处理器
    void operator()(crow::request& req, crow::response& res, context& ctx) {
        // 从Cookie中获取session_id
        std::string session_id = get_session_id_from_cookies(req.get_header_value("Cookie"));

        if (!session_id.empty()) {
            auto session = session_manager_.get_session(session_id);
            if (session && session->is_admin) {
                ctx.is_admin = true;
                ctx.session = session;
            }
        }

        // 继续处理请求
        res.end();
    }

    // 从Cookie字符串中提取session_id
    std::string get_session_id_from_cookies(const std::string& cookie_header) {
        if (cookie_header.empty()) return "";

        size_t session_start = cookie_header.find("session_id=");
        if (session_start == std::string::npos) return "";

        session_start += 10; // "session_id=".length()
        size_t session_end = cookie_header.find(';', session_start);

        if (session_end == std::string::npos) {
            return cookie_header.substr(session_start);
        } else {
            return cookie_header.substr(session_start, session_end - session_start);
        }
    }

    // 验证密码
    bool verify_password(const std::string& password) {
        return session_manager_.verify_password(password, admin_password_);
    }

    // 创建管理员会话
    std::shared_ptr<SessionManager::Session> create_admin_session(const std::string& user_id = "admin") {
        return session_manager_.create_admin_session(user_id);
    }

    // 销毁会话
    void destroy_session(const std::string& session_id) {
        session_manager_.destroy_session(session_id);
    }

private:
    SessionManager session_manager_;
    std::string admin_password_;
};

#endif // AUTH_MIDDLEWARE_HPP