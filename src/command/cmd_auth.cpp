#include "command/command.h"

#include "user/user_registry.h"

#include <string>

namespace redis_clone {
namespace {

const std::string& stringArgument(const RespValue::Array& arguments,
                                  std::size_t index) {
    return std::get<BulkString>(arguments[index].data).value;
}

}  // namespace

void cmd_auth(CommandContext& context, const RespValue::Array& arguments) {
    if (context.authenticatedUser_ != nullptr) {
        RespWriter::writeError(context.output,
                               "ERR already authenticated on this connection");
        return;
    }

    User* user = context.users_.find(stringArgument(arguments, 1));
    if (user == nullptr ||
        !context.users_.verifyPassword(*user, stringArgument(arguments, 2))) {
        RespWriter::writeError(
            context.output,
            "WRONGPASS invalid username-password pair or user is disabled");
        return;
    }
    if (!context.users_.claimSession(*user, context.clientId_)) {
        RespWriter::writeError(context.output,
                               "ERR user already has an active session");
        return;
    }

    context.authenticatedUser_ = user;
    RespWriter::writeOk(context.output);
}

void cmd_whoami(CommandContext& context, const RespValue::Array&) {
    RespWriter::writeBulk(context.output, context.authenticatedUser_->name);
}

}  // namespace redis_clone
