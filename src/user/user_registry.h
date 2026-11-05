#pragma once

#include "store/database.h"

#include <cstdint>
#include <chrono>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace redis_clone {

struct User {
    std::string name;
    Database database;

private:
    std::string password;
    std::uint64_t sessionClientId = 0;

    friend class UserRegistry;
};

class UserRegistry {
public:
    User& addUser(std::string name, std::string password);
    User* find(std::string_view name);
    const User* find(std::string_view name) const;
    bool verifyPassword(const User& user, std::string_view password) const noexcept;
    bool claimSession(User& user, std::uint64_t clientId) noexcept;
    void releaseSession(User& user, std::uint64_t clientId) noexcept;
    std::size_t activeExpireCycle(
        std::chrono::steady_clock::duration totalBudget);

    static UserRegistry loadFromFile(const std::string& path);

private:
    std::unordered_map<std::string, std::unique_ptr<User>> users_;
    std::vector<User*> expiryUsers_;
    std::size_t nextExpiryUser_ = 0;
};

}  // namespace redis_clone
