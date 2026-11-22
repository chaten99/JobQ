#include "EventRoutes.h"

#include "EventHub.h"
#include "ServerRuntime.h"
#include <drogon/WebSocketController.h>

namespace jobq::server
{

    class EventSocket final : public drogon::WebSocketController<EventSocket, false>
    {
    public:
        explicit EventSocket(const std::shared_ptr<ServerContext> &context) : context_(context) {}

        static void initPathRouting() {}

        void handleNewConnection(const drogon::HttpRequestPtr &request,
                                 const drogon::WebSocketConnectionPtr &connection) override
        {
            connection->disablePing();
            try
            {
                const auto context = context_.lock();
                if (context && context->ready() && request->query().empty() &&
                    context->events->subscribe(connection))
                    return;
            }
            catch (...)
            {
            }
            connection->forceClose();
        }

        void handleNewMessage(const drogon::WebSocketConnectionPtr &connection, std::string &&message,
                              const drogon::WebSocketMessageType &type) override
        {
            const auto context = context_.lock();
            if (!context)
                return;
            if (type == drogon::WebSocketMessageType::Pong)
            {
                context->events->acknowledge(connection, message);
            }
            else if (type != drogon::WebSocketMessageType::Ping && type != drogon::WebSocketMessageType::Close)
            {
                context->events->remove(connection);
                connection->forceClose();
            }
        }

        void handleConnectionClosed(const drogon::WebSocketConnectionPtr &connection) override
        {
            if (const auto context = context_.lock())
                context->events->remove(connection);
        }

    private:
        std::weak_ptr<ServerContext> context_;
    };

    void registerEventRoutes(const std::shared_ptr<ServerContext> &context)
    {
        drogon::app().registerController(std::make_shared<EventSocket>(context));
        drogon::app().registerWebSocketController("/api/v1/events", EventSocket::classTypeName(), {drogon::Get});
    }

}
