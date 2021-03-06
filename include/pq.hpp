#ifndef MPL_PQ_HPP
#define MPL_PQ_HPP

#include <string>
#include <vector>
#include <amqp.h>
#include <amqp_tcp_socket.h>
#include "jilog.hpp"
#include <stdio.h> // To format AMQP errors
#include <util.hpp>

class GenericMessageQueue {
public:
    void put();
    template<class T> T get();
};

class RabbitMQMessageQueue: public GenericMessageQueue {
    RabbitMQMessageQueue();
    char const *hostname;
    int port, status;
    char const *exchange = "amq.direct";
    char const *routingkey;
    amqp_socket_t *socket = NULL;
    amqp_connection_state_t conn;


    void initializeChannel() {
        conn = amqp_new_connection();

        socket = amqp_tcp_socket_new(conn);
        if (!socket) {
            JI_LOG(ERROR) << "Cannot open tcp socket";
            exit(1);
        }

        status = amqp_socket_open(socket, hostname, port);
        die_on_amqp_error(amqp_login(conn, "/", 0, 131072, 0, AMQP_SASL_METHOD_PLAIN,
                                     "guest", "guest"), "Logging in");
        amqp_channel_open(conn, 1);
        die_on_amqp_error(amqp_get_rpc_reply(conn), "Opening channel");


        amqp_queue_declare_ok_t *r = amqp_queue_declare(
                conn, 1, amqp_cstring_bytes(routingkey), 0, 0, 0, 0, amqp_empty_table);
        die_on_amqp_error(amqp_get_rpc_reply(conn), "Declaring queue");
        amqp_bytes_t queuename;
        queuename = amqp_bytes_malloc_dup(r->queue);
        if (queuename.bytes == NULL) {
            fprintf(stderr, "Out of memory while copying queue name");
            exit(1);
        }

        amqp_queue_bind(conn, 1, queuename, amqp_cstring_bytes(exchange),
                        amqp_cstring_bytes(routingkey), amqp_empty_table);
        die_on_amqp_error(amqp_get_rpc_reply(conn), "Binding queue");

        amqp_basic_consume(conn, 1, queuename, amqp_empty_bytes, 0, 1, 0,
                           amqp_empty_table);
        die_on_amqp_error(amqp_get_rpc_reply(conn), "Consuming");

    }

    void die_on_amqp_error(amqp_rpc_reply_t x, char const *context) {
        // TODO: change these to use JI_LOG
        switch (x.reply_type) {
            case AMQP_RESPONSE_NORMAL:
                //JI_LOG(DEBUG) << context;
                return;

            case AMQP_RESPONSE_NONE:
                fprintf(stderr, "%s: missing RPC reply type!\n", context);
                break;

            case AMQP_RESPONSE_LIBRARY_EXCEPTION:
                fprintf(stderr, "%s: %s\n", context, amqp_error_string2(x.library_error));
                break;

            case AMQP_RESPONSE_SERVER_EXCEPTION:
                switch (x.reply.id) {
                    case AMQP_CONNECTION_CLOSE_METHOD: {
                        amqp_connection_close_t *m =
                                (amqp_connection_close_t *)x.reply.decoded;
                        fprintf(stderr, "%s: server connection error %uh, message: %.*s\n",
                                context, m->reply_code, (int)m->reply_text.len,
                                (char *)m->reply_text.bytes);
                        break;
                    }
                    case AMQP_CHANNEL_CLOSE_METHOD: {
                        amqp_channel_close_t *m = (amqp_channel_close_t *)x.reply.decoded;
                        fprintf(stderr, "%s: server channel error %uh, message: %.*s\n",
                                context, m->reply_code, (int)m->reply_text.len,
                                (char *)m->reply_text.bytes);
                        break;
                    }
                    default:
                        fprintf(stderr, "%s: unknown server error, method id 0x%08X\n",
                                context, x.reply.id);
                        break;
                }
                break;
        }

        exit(1);

    }

public:
    RabbitMQMessageQueue(char const * hostname, int port, char const * routingkey) {
        this->routingkey = routingkey;
        this->hostname = hostname;
        this->port = port;
        initializeChannel();
    }


    RabbitMQMessageQueue(std::string hostname_and_port, char const * routingkey) {

        this->routingkey = routingkey;
        auto colon_position = hostname_and_port.find(":");
        hostname = hostname_and_port.substr(0, colon_position).c_str();
        port = stoi(hostname_and_port.substr(colon_position + 1, 4));

//        this->hostname = hostname;
//        this->port = port;
        initializeChannel();
    }

    void poll() { } // Not needed for this kind of communicator
    void flush() { } // Not needed for this kind of communicator

    void set_destination(char const * routingkey_) {
        routingkey = routingkey_;

        // Make sure the queue exists for gets
        amqp_queue_declare_ok_t *r = amqp_queue_declare(
                conn, 1, amqp_cstring_bytes(routingkey), 0, 0, 0, 0, amqp_empty_table);
        die_on_amqp_error(amqp_get_rpc_reply(conn), "Declaring queue");
        amqp_bytes_t queuename;
        queuename = amqp_bytes_malloc_dup(r->queue);
        if (queuename.bytes == NULL) {
            fprintf(stderr, "Out of memory while copying queue name");
            exit(1);
        }

        amqp_queue_bind(conn, 1, queuename, amqp_cstring_bytes(exchange),
                        amqp_cstring_bytes(routingkey), amqp_empty_table);
        die_on_amqp_error(amqp_get_rpc_reply(conn), "Binding queue");

        amqp_basic_consume(conn, 1, queuename, amqp_empty_bytes, 0, 1, 0,
                           amqp_empty_table);
        die_on_amqp_error(amqp_get_rpc_reply(conn), "Consuming");

    }

    template<class T>
    std::vector<T> get(std::string routingkey_, int max_num_messages) {
        std::vector<T> ret;
        for(int i=0; i < max_num_messages; ++i) {
            ret.emplace_back(get<T>(routingkey_));
        }
        return ret;
    }

    template<class T>
    void put(T& message, std::string routingkey_) {
        set_destination(routingkey_.c_str());
        std::string message_body = message.serialize();
        //amqp_basic_properties_t props;
        //props._flags = AMQP_BASIC_CONTENT_TYPE_FLAG | AMQP_BASIC_DELIVERY_MODE_FLAG;
        //props.content_type = amqp_cstring_bytes("text/plain");
        //props.delivery_mode = 2;// persistent delivery mode
        int response = amqp_basic_publish(conn, 1, amqp_cstring_bytes(exchange),
                                          amqp_cstring_bytes(routingkey), 0, 0,
                                          NULL, amqp_cstring_bytes(message_body.c_str()));
        if (response < 0) {
            JI_LOG(ERROR) << "Error sending message: " << message_body; //TODO: truncate message
        } else {
//            JI_LOG(INFO) << "Succesfully sent message: " << message_body << " to routingkey " << routingkey; //TODO: truncate message
        }
    }

    void put_raw(std::string message, std::string routingkey_) {
        set_destination(routingkey_.c_str());
        std::string message_body = message;
        //amqp_basic_properties_t props;
        //props._flags = AMQP_BASIC_CONTENT_TYPE_FLAG | AMQP_BASIC_DELIVERY_MODE_FLAG;
        //props.content_type = amqp_cstring_bytes("text/plain");
        //props.delivery_mode = 2;// persistent delivery mode
        int response = amqp_basic_publish(conn, 1, amqp_cstring_bytes(exchange),
                                          amqp_cstring_bytes(routingkey), 0, 0,
                                          NULL, amqp_cstring_bytes(message_body.c_str()));
        if (response < 0) {
            JI_LOG(ERROR) << "Error sending message: " << message_body; //TODO: truncate message
        } else {
            JI_LOG(INFO) << "Succesfully sent message: " << message_body << " to routingkey " << routingkey; //TODO: truncate message
        }
    }

    std::string get_raw(std::string routingkey_) {
        set_destination(routingkey_.c_str());
        amqp_rpc_reply_t res;
        amqp_envelope_t envelope;

        amqp_maybe_release_buffers(conn);
        timeval consume_timeout;
        consume_timeout.tv_sec = 0.9;
        consume_timeout.tv_usec = 0;
//        consume_timeout.tv_sec = 0.2;

        res = amqp_consume_message(conn, &envelope, &consume_timeout, 0);

        if (AMQP_RESPONSE_NORMAL != res.reply_type) {
            JI_LOG(ERROR) << "Abnormal response from queue";
            return "";
        }

        std::string ret_string((char const *)envelope.message.body.bytes, envelope.message.body.len);

        amqp_destroy_envelope(&envelope);
        return ret_string;

    }

    template<class T>
    T get(std::string routingkey_) {
        set_destination(routingkey_.c_str());
        amqp_rpc_reply_t res;
        amqp_envelope_t envelope;

        amqp_maybe_release_buffers(conn);
        const timeval consume_timeout{static_cast<__darwin_time_t>(0.2)};
//        consume_timeout.tv_sec = 0.2;

        res = amqp_consume_message(conn, &envelope, &consume_timeout, 0);

        if (AMQP_RESPONSE_NORMAL != res.reply_type) {
            JI_LOG(ERROR) << "Abnormal response from queue";
            return T();
        }

        std::string ret_string((char const *)envelope.message.body.bytes, envelope.message.body.len);

        amqp_destroy_envelope(&envelope);
        return T::deserialize(ret_string);
    }

//    template<class T>
//    T get() {
//        amqp_rpc_reply_t res;
//        amqp_envelope_t envelope;
//
//        amqp_maybe_release_buffers(conn);
//
//        res = amqp_consume_message(conn, &envelope, NULL, 0);
//
//        if (AMQP_RESPONSE_NORMAL != res.reply_type) {
//            JI_LOG(ERROR) << "Abnormal response from queue";
//            return T();
//        }
//
//        //printf("Delivery %u, exchange %.*s routingkey %.*s\n",
//        //    (unsigned)envelope.delivery_tag, (int)envelope.exchange.len,
//        //    (char *)envelope.exchange.bytes, (int)envelope.routing_key.len,
//        //    (char *)envelope.routing_key.bytes);
//
//        //if (envelope.message.properties._flags & AMQP_BASIC_CONTENT_TYPE_FLAG) {
//        //  printf("Content-type: %.*s\n",
//        //      (int)envelope.message.properties.content_type.len,
//        //      (char *)envelope.message.properties.content_type.bytes);
//        //}
//        //printf("----\n");
//
//        //amqp_dump(envelope.message.body.bytes, envelope.message.body.len);
//        //T ret_obj(envelope.message.body.)
//        std::string ret_string((char const *)envelope.message.body.bytes, envelope.message.body.len);
//
//        amqp_destroy_envelope(&envelope);
//        return T(ret_string);
//
//    }

    ~RabbitMQMessageQueue() {
        // This causes errors, omit for now and allow garbage collection to take care
//        die_on_amqp_error(amqp_channel_close(conn, 1, AMQP_REPLY_SUCCESS),
//                          "Closing channel");
//        die_on_amqp_error(amqp_connection_close(conn, AMQP_REPLY_SUCCESS),
//                          "Closing connection");
//        int response = amqp_destroy_connection(conn);
//        if (response < 0) {
//            JI_LOG(ERROR) << "Error destroying connection";
//            exit(1);
//        }
    }

};

#endif
