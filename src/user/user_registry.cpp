#include "user/user_registry.h"

#include <cctype>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace redis_clone {
namespace {

bool hasWhitespace(std::string_view value) {
    for (const unsigned char character : value) {
        if (std::isspace(character) != 0) {
            return true;
        }
    }
    return false;
}

}  // namespace

User& UserRegistry::addUser(std::string name, std::string password) {
    if (name.empty() || password.empty() || hasWhitespace(name) ||
        hasWhitespace(password)) {
        throw std::invalid_argument(
            "user names and passwords must be non-empty and contain no whitespace");
    }
    auto user = std::make_unique<User>();
    user->name = std::move(name);
    user->password = std::move(password);
    const std::string key = user->name;
    const auto inserted = users_.emplace(key, std::move(user));
    if (!inserted.second) {
        throw std::invalid_argument("duplicate user name in users configuration");
    }
    try {
        expiryUsers_.push_back(inserted.first->second.get());
    } catch (...) {
        users_.erase(inserted.first);
        throw;
    }
    return *inserted.first->second;
}

User* UserRegistry::find(std::string_view name) {
    const auto found = users_.find(std::string(name));
    return found == users_.end() ? nullptr : found->second.get();
}

const User* UserRegistry::find(std::string_view name) const {
    const auto found = users_.find(std::string(name));
    return found == users_.end() ? nullptr : found->second.get();
}

bool UserRegistry::verifyPassword(const User& user,
                                 std::string_view password) const noexcept {
    return user.password == password;
}

bool UserRegistry::claimSession(User& user, std::uint64_t clientId) noexcept {
    if (clientId == 0 || user.sessionClientId != 0) {
        return false;
    }
    user.sessionClientId = clientId;
    return true;
}

void UserRegistry::releaseSession(User& user, std::uint64_t clientId) noexcept {
    if (user.sessionClientId == clientId) {
        user.sessionClientId = 0;
    }
}

std::size_t UserRegistry::activeExpireCycle(
    std::chrono::steady_clock::duration totalBudget) {
    if (expiryUsers_.empty() ||
        totalBudget <= std::chrono::steady_clock::duration::zero()) {
        return 0;
    }

    const auto startedAt = std::chrono::steady_clock::now();
    std::size_t removed = 0;
    for (std::size_t visited = 0;
         visited < expiryUsers_.size() &&
         std::chrono::steady_clock::now() - startedAt < totalBudget;
         ++visited) {
        User* user = expiryUsers_[nextExpiryUser_];
        nextExpiryUser_ = (nextExpiryUser_ + 1) % expiryUsers_.size();
        const auto elapsed = std::chrono::steady_clock::now() - startedAt;
        removed += user->database.activeExpireCycle(totalBudget - elapsed);
    }
    return removed;
}

UserRegistry UserRegistry::loadFromFile(const std::string& path) {
    std::ifstream input(path);
    if (!input) {
        throw std::runtime_error("could not open users file: " + path);
    }

    UserRegistry registry;
    std::string line;
    std::size_t lineNumber = 0;
    while (std::getline(input, line)) {
        ++lineNumber;
        std::istringstream fields(line);
        std::string name;
        std::string password;
        std::string extra;
        if (!(fields >> name)) {
            continue;
        }
        if (name.front() == '#') {
            continue;
        }
        if (!(fields >> password) || (fields >> extra)) {
            throw std::runtime_error("invalid users file entry on line " +
                                     std::to_string(lineNumber));
        }
        try {
            registry.addUser(std::move(name), std::move(password));
        } catch (const std::invalid_argument& error) {
            throw std::runtime_error("invalid users file entry on line " +
                                     std::to_string(lineNumber) + ": " +
                                     error.what());
        }
    }
    if (input.bad()) {
        throw std::runtime_error("error reading users file: " + path);
    }
    return registry;
}

}  // namespace redis_clone
