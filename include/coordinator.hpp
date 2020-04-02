
#ifndef MPL_COORDINATOR_HPP
#define MPL_COORDINATOR_HPP

#include <string>
#include <vector>
#include <demo/app_options.hpp>
#include <jilog.hpp>
#include <fcntl.h>
#include <unistd.h>
#include <system_error>
#include <sys/errno.h>
#include <subspace.hpp>
#include <graph.hpp>
#include <tree.hpp>
#include <prm_planner.hpp>
#include <message.hpp>
#include <connection.hpp>
#include <netinet/ip.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <syserr.hpp>


namespace mpl {
    struct LambdaType {
        constexpr static auto LAMBDA_PSEUDO = "local";
    };

    template <class Scenario, class Scalar>
    class CoordinatorFixedGraph {
    public:
        using State = typename Scenario::State;
        using Bound = typename Scenario::Bound;
        using Distance = typename Scenario::Distance;
        using Subspace_t = Subspace<Bound, State, Scalar>;
        using Planner = mpl::PRMPlanner<Scenario, Scalar>;
        using Vertex = mpl::Vertex<State>;
        using Edge = mpl::Edge<typename Vertex::ID, Distance>;
        using Graph = UndirectedGraph<Vertex, Edge>;
        using Connection_t = Connection<CoordinatorFixedGraph>;

        std::unordered_map<std::uint64_t, std::vector<Buffer>> buffered_data_; // Any data to be written if lambda is not up yet
        int smallest_number_of_neighbors;

    private:
        std::vector<std::uint64_t> num_samples_per_lambda_;
        std::uint64_t global_min_samples;
        demo::AppOptions app_options;
        Subspace_t global_subspace;
        std::vector<Subspace_t> lambda_subspaces;
        int listen_{-1};
        int port_{0x415E};
        std::vector<struct pollfd> pfds;
        std::list<Connection_t> connections_;
        std::list<std::pair<int, int>> childProcesses_;
        std::unordered_map<std::uint64_t, Connection_t*> lambdaId_to_connection_;
        Graph graph;

        void init_local_lambdas() {
            for (int i=0; i < app_options.jobs() ; ++i) {
                int p[2];
                if (::pipe(p) == -1)
                    throw std::system_error(errno, std::system_category(), "Pipe");
                if (int pid = ::fork()) {
                    // parent process
                    ::close(p[1]);
                    continue;
                }
                ::close(p[0]);

                std::string program = "./mpl_lambda_fixed_graph";
                auto lambdaId = std::to_string(i);
                auto lambda_min = lambda_subspaces[i].getLower();
                auto lambda_max = lambda_subspaces[i].getUpper();
                std::vector<std::string> args = {
                        "--scenario", app_options.scenario(),
                        "--global_min", app_options.global_min_,
                        "--global_max", app_options.global_max_,
//                        "--min", std::to_string(lambda_min[0]) + "," + std::to_string(lambda_min[1]),
//                        "--max", std::to_string(lambda_max[0]) + "," + std::to_string(lambda_max[1]),
                        "--lambda_id", lambdaId,
                        "--algorithm", app_options.algorithm(),
                        "--coordinator", app_options.coordinator(),
                        "--communicator", app_options.communicator(),
                        "--num_samples", std::to_string(app_options.numSamples()),
                        "--time-limit", std::to_string(app_options.timeLimit()),
                        "--env", app_options.env(false),
                        "--env-frame", app_options.envFrame_,
//                        "--start", app_options.start_,
//                        "--goal", app_options.goal_,
                        "--num_divisions", app_options.num_divisions_
//                    "--robot", app_options.robot()
                };
                for (int i=0; i < app_options.starts_.size(); ++i) {
                    args.push_back("--start");
                    args.push_back(app_options.starts_[i]);
                    args.push_back("--goal");
                    args.push_back(app_options.goals_[i]);
                }


                std::vector <const char *> argv;

                char file[20];
                snprintf(file, sizeof(file), "lambda-%d.out", i);

                argv.push_back(program.c_str());
                std::string command = program;
//                    for (auto i=0; i + 1 < args.size(); i += 2) {
//                        command += " " + args[i]+ "=" + args[i+1];
//                        argv.push_back((args[i]+ "=" + args[i+1]).c_str());
                for (auto ind=0; ind < args.size(); ++ind) {
                    argv.push_back(args[ind].c_str());
                    command += " " + args[ind];
                }
                argv.push_back(nullptr); // <-- required terminator
                JI_LOG(TRACE) << "Running lambda" << lambdaId << " file " << file << ": " << command;
//                    for (auto c : argv) {
//                        JI_LOG(INFO) << c;
//                    }

                int fd = ::open(file, O_RDWR | O_CREAT | O_TRUNC, S_IRUSR | S_IWUSR);
                dup2(fd, 1); // make stdout write to file
                dup2(fd, 2); // make stderr write to file
                close(fd); // close fd, dups remain open
                execv(argv[0], const_cast<char*const*>(argv.data()));
                throw std::system_error(errno, std::system_category(), "Lambda died");
            }

        }
    public:
        CoordinatorFixedGraph (demo::AppOptions& app_options_) :
                app_options(app_options_),
                global_subspace(Subspace_t(app_options_.globalMin<Bound>(), app_options_.globalMax<Bound>()))
        {
            smallest_number_of_neighbors = (int) (pow(2, global_subspace.dimension()) - 1);
        }

        ~CoordinatorFixedGraph() {
            if (listen_ != -1 && !::close(listen_))
                JI_LOG(WARN) << "failed to close listening socket";
        }

        std::string algorithm() {
            return "prm_fixed_graph";
        }

        void addVertices(const std::vector<Vertex>& vertices) {
            for (auto& v : vertices) {
                graph.addVertex(v);
            }
        }

        void update_num_samples(std::uint64_t lambdaId, size_t successful_samples) {
            num_samples_per_lambda_[lambdaId] += app_options.numSamples(); // Regardless of how many successful samples, each lambda tries to sample these many points
            auto curr_min_samples = std::min_element(
                    num_samples_per_lambda_.begin(),
                    num_samples_per_lambda_.end()
            );
            if ((*curr_min_samples) > global_min_samples) {
                global_min_samples = *curr_min_samples;
                JI_LOG(INFO) << "Updating global_num_samples to " << global_min_samples * app_options.jobs();
                for (auto& c: connections_) {
                    c.write(packet::NumSamples(global_min_samples * app_options.jobs()));
                }
            }
        }

        void addEdges(const std::vector<Edge>& edges) {
            for (auto& e : edges) {
                graph.addEdge(e);
            }
        }

        const Graph& getGraph() const {
            return graph;
        };

        const std::vector<Subspace_t> getSubspaces() const {
            return lambda_subspaces;
        }


        void start_socket() {
            if ((listen_ = ::socket(PF_INET, SOCK_STREAM, 0)) == -1)
                throw syserr("socket()");

            int on = 1;
            if (::setsockopt(listen_, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on)) == -1)
                throw syserr("set reuse addr");

            struct sockaddr_in addr;
            addr.sin_family = AF_INET;
            addr.sin_addr.s_addr = htonl(INADDR_ANY);
            addr.sin_port = htons(port_);

            if (::bind(listen_, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) == -1)
                throw syserr("bind()");

            socklen_t addrLen = sizeof(addr);
            if (::getsockname(listen_, reinterpret_cast<struct sockaddr*>(&addr), &addrLen) == -1)
                throw syserr("getsockname()");

            JI_LOG(INFO) << "listening on port: " << ntohs(addr.sin_port);

            if (::listen(listen_, 10000) == -1)
                throw syserr("listen()");
        }

        void divide_work() {
            int num_jobs = app_options.jobs();
            int dimension = global_subspace.dimension();
            int num_divisions_per_axis = pow(num_jobs, 1.0 / dimension) - 1;
            std::vector<int> num_divisions;
            app_options.num_divisions_ = "";
            for (int i=0; i < dimension; ++i) {
                num_divisions.push_back(num_divisions_per_axis);
                app_options.num_divisions_ += std::to_string(num_divisions_per_axis) + ",";
            }
            app_options.num_divisions_ = app_options.num_divisions_.substr(0, app_options.num_divisions_.length() - 1);
//            auto n = global_subspace.layered_divide(num_divisions);
//            JI_LOG(INFO) << n;
            lambda_subspaces = global_subspace.divide(num_divisions);
            assert(lambda_subspaces.size() == num_jobs);
            JI_LOG(INFO) << "Num lambdas: " << app_options.jobs();
            JI_LOG(INFO) << "Global Space: " << global_subspace;
            JI_LOG(INFO) << "Subspaces: " << lambda_subspaces;
        }

        std::vector<State> solve() {
        }

        std::vector<std::pair<Vertex, Vertex>> getStartAndGoalVertices(const std::vector<std::pair<State, State>> &starts_and_goals) {
            // We can avoid querying each vertex to see if it is the start or the goal because we
            // add it to a lambda only if it contains it. Moreover, each lambda starts off by adding all the
            // start and goal points in the same order.
            std::vector<int> lambda_current_vertex_id;
            std::vector<std::pair<Vertex, Vertex>> start_and_goal_vertex;
            lambda_current_vertex_id.resize(lambda_subspaces.size(), 0);
            for (auto& [start, goal] : starts_and_goals) {
                std::string start_id, goal_id;
                for (int lambda_id=0; lambda_id < lambda_subspaces.size(); ++lambda_id) {
                  if (lambda_subspaces[lambda_id].contains(start)) {
                    start_id = std::to_string(lambda_id) + "_" + std::to_string(lambda_current_vertex_id[lambda_id]++);
                  }
                  if (lambda_subspaces[lambda_id].contains(goal)) {
                    goal_id = std::to_string(lambda_id) + "_" + std::to_string(lambda_current_vertex_id[lambda_id]++);
                  }
                }
                start_and_goal_vertex.push_back(std::make_pair(
                        Vertex{start_id, start},
                        Vertex{goal_id, goal}
                      ));
            }
            return start_and_goal_vertex;
        }

        void loop() {
            // Do communication stuff// TODO: handle case where other connection not initiated
            bool done_ = false;
            auto start_and_goal_states = app_options.getStartsAndGoals<State>();
            auto start_and_goal_vertices = getStartAndGoalVertices(start_and_goal_states);
            for (auto& [start, goal] : start_and_goal_vertices) {
                JI_LOG(INFO) << "Using start " << start.id() << " and goal " << goal.id();
            }
            auto start = std::chrono::high_resolution_clock::now();
            for(;;) {


                pfds.clear();
                // first comes the fds of the child processes (note that
                // connection processing may change the child process list, so
                // this must be processed first)
                for (auto[pid, fd] : childProcesses_) {
                    pfds.emplace_back();
                    pfds.back().fd = fd;
                    pfds.back().events = POLLIN;
                }

                // then et of pollfds is 1:1 with connections
                for (Connection_t &conn : connections_)
                    pfds.emplace_back(conn);

                // then comes the accepting socket
                pfds.emplace_back();
                pfds.back().fd = listen_;
                pfds.back().events = POLLIN;

//                JI_LOG(TRACE) << "polling " << pfds.size();
                int nReady = ::poll(pfds.data(), pfds.size(), 1);
                auto stop = std::chrono::high_resolution_clock::now();
                auto duration = std::chrono::duration_cast<std::chrono::seconds>(stop - start);
                if (duration.count() > app_options.timeLimit()) {
                    done_ = true;
                    for (auto& c : connections_) {
                        c.sendDone();
                    }
                }

//                JI_LOG(TRACE) << "poll returned " << nReady;

                if (nReady == -1) {
                    if (errno == EAGAIN || errno == EINTR) {
                        continue;
                    } else {
                        throw syserr("poll");
                    }

                }

                auto pit = pfds.begin();
                for (auto cit = childProcesses_.begin(); cit != childProcesses_.end(); ++pit) {
                    assert(pit != pfds.end());

                    if ((pit->revents & POLLHUP) == 0) {
                        ++cit;
                    } else {
                        int stat = 0;
                        // if (::waitpid(cit->first, &stat, 0) == -1)
                        //     JI_LOG(WARN) << "waitpid failed with error: " << errno;
                        if (::close(cit->second) == -1)
                            JI_LOG(WARN) << "close failed with error: " << errno;
                        JI_LOG(INFO) << "child process " << cit->first << " exited with status " << stat;
                        cit = childProcesses_.erase(cit);
                    }
                }

                for (auto cit = connections_.begin(); cit != connections_.end(); ++pit) {
                    assert(pit != pfds.end());

                    if (cit->process(*pit)) {
                        if (cit->recvHello() &&
                            (lambdaId_to_connection_.find(cit->lambdaId())) == lambdaId_to_connection_.end()) {
                            lambdaId_to_connection_[cit->lambdaId()] = &(*cit);
                            for (auto& buf: buffered_data_[cit->lambdaId()]) {
                                cit->write(std::move(buf));
                            }
                        }
                        ++cit;
                    }
                    else {
                        if (cit->recvHello()) {
                            // it should be in the map
                            lambdaId_to_connection_.erase(cit->lambdaId());
                        }
                        cit = connections_.erase(cit);
                    }
                }

                assert(pit + 1 == pfds.end());

                if (pit->revents & (POLLERR | POLLHUP))
                    break;
                if (pit->revents & POLLIN) {
                    JI_LOG(INFO) << "new connection";
                    connections_.emplace_back(*this);
                    if (!connections_.back()) {
                        JI_LOG(WARN) << "accept failed with error: " << errno;
                        connections_.pop_back();
                    }
                    // struct sockaddr_in addr;
                    // socklen_t addLen = sizeof(addr);

                    // int fd = ::accept(listen, &addr, &addrLen);
                    // if (fd != -1)
                    //     connections.emplace_back(socket_);
                    // && !connections.emplace_back(listen_))
                    // connections.pop_back();
                }

                if (done_) break;



                // Get new vertices and edges from lambdas
//            auto vector_new_vertices= comm.template get<VectorMessage<Vertex>>("graph_vertices");
//            auto vector_new_edges = comm.template get<VectorMessage<Edge>>(std::string("graph_edges"));

                // Compute new rPRM
                // Ensure vertices broadcast to all lambdas

            }
            JI_LOG(INFO) << "Loop done";
            connections_.clear();
            // Handle cleaning up lambdas
        }

        void init_lambdas() {
            if (app_options.lambdaType() == LambdaType::LAMBDA_PSEUDO) {
                init_local_lambdas();
            }

            int num_lambdas = app_options.jobs();
            num_samples_per_lambda_.resize(num_lambdas, 0);
        }

        Connection_t* getConnection(std::uint64_t lambdaId) {
            return lambdaId_to_connection_[lambdaId];
        }

        int accept(struct sockaddr *addr, socklen_t * addrLen) {
            return ::accept(listen_, addr, addrLen);
        }


    };
}


#endif //MPL_COORDINATOR_HPP
