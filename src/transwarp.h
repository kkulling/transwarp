#pragma once
#include <future>
#include <type_traits>
#include <memory>
#include <tuple>
#include <string>
#include <cstddef>
#include <vector>
#include <functional>
#include <thread>
#include <algorithm>
#include "cxxpool.h"


namespace transwarp {


struct node {
    std::size_t id;
    std::string name;
};


struct edge {
    transwarp::node child;
    transwarp::node parent;
};


namespace detail {

template<bool Done, int Total, int... N>
struct call_impl {
    template<typename Result, typename F, typename Tuple>
    static Result call(F&& f, Tuple&& t) {
        return call_impl<Total == 1 + sizeof...(N), Total, N..., sizeof...(N)>::template
                call<Result>(std::forward<F>(f), std::forward<Tuple>(t));
    }
};

template<int Total, int... N>
struct call_impl<true, Total, N...> {
    template<typename Result, typename F, typename Tuple>
    static Result call(F&& f, Tuple&& t) {
        return std::forward<F>(f)(std::get<N>(std::forward<Tuple>(t))->get_future().get()...);
    }
};

template<typename Result, typename F, typename Tuple>
Result call(F&& f, Tuple&& t) {
    using ttype = typename std::decay<Tuple>::type;
    return transwarp::detail::call_impl<0 == std::tuple_size<ttype>::value, std::tuple_size<ttype>::value>::template
            call<Result>(std::forward<F>(f), std::forward<Tuple>(t));
}

template<size_t ...> struct indices
{};

template<size_t ...> struct construct_range;

template< size_t end, size_t idx, size_t ...i >
struct construct_range<end, idx, i... >
     : construct_range<end, idx+1, i..., idx> {};

template< size_t end, size_t ...i >
struct construct_range< end, end, i... > {
    typedef transwarp::detail::indices< i... > type;
};

template<size_t b, size_t e>
struct index_range {
    typedef typename transwarp::detail::construct_range<e, b>::type type;
};

template<typename F, typename T, typename ...Args>
void tuple_for_each_index(transwarp::detail::indices<>, const F&, T&, const Args&...)
{}

template<typename F, typename T, typename ...Args>
void tuple_for_each_index(transwarp::detail::indices<>, const F&, const T&, const Args&...)
{}

template<size_t i, size_t ...j, typename F, typename T, typename ...Args>
void tuple_for_each_index(transwarp::detail::indices<i,j...>, const F& f, T& t, const Args&... args) {
    f(std::get<i>(t).get(), args...);
    transwarp::detail::tuple_for_each_index(transwarp::detail::indices<j...>(), f, t, args...);
}

template<size_t i, size_t ...j, typename F, typename T, typename ...Args>
void tuple_for_each_index(transwarp::detail::indices<i,j...>, const F& f, const T& t, const Args&... args) {
    f(std::get<i>(t).get(), args...);
    transwarp::detail::tuple_for_each_index(transwarp::detail::indices<j...>(), f, t, args...);
}

template<typename F, typename T, typename ...Args>
void apply(const F& f, T& t, const Args&... args) {
    static const size_t n = std::tuple_size<T>::value;
    typedef typename transwarp::detail::index_range<0,n>::type index_list;
    transwarp::detail::tuple_for_each_index(index_list(), f, t, args...);
}

template<typename F, typename T, typename ...Args>
void apply(const F& f, const T& t, const Args&... args) {
    static const size_t n = std::tuple_size<T>::value;
    typedef typename transwarp::detail::index_range<0,n>::type index_list;
    transwarp::detail::tuple_for_each_index(index_list(), f, t, args...);
}

inline
std::string trim(const std::string &s, const std::string& chars=" \t\n\r") {
    auto functor = [&chars](char c) { return chars.find(c) != std::string::npos; };
    auto it = std::find_if_not(s.begin(), s.end(), functor);
    return std::string(it, std::find_if_not(s.rbegin(), std::string::const_reverse_iterator(it), functor).base());
}

struct unvisit_functor {
    template<typename Task>
    void operator()(Task* task) const {
        task->unvisit();
    }
};

struct make_edges_functor {
    make_edges_functor(std::vector<transwarp::edge>& graph, transwarp::node n)
    : graph_(graph), n_(std::move(n))
    {}
    template<typename Task>
    void operator()(Task* task) const {
        graph_.push_back({n_, task->get_node()});
    }
    std::vector<transwarp::edge>& graph_;
    transwarp::node n_;
};

template<typename PreVisitor, typename PostVisitor>
struct visit_functor {
    visit_functor(PreVisitor& pre_visitor, PostVisitor& post_visitor)
    : pre_visitor_(pre_visitor), post_visitor_(post_visitor) {}
    template<typename Task>
    void operator()(Task* task) const {
        task->visit(pre_visitor_, post_visitor_);
    }
    PreVisitor& pre_visitor_;
    PostVisitor& post_visitor_;
};

struct id_visitor {
    explicit id_visitor(std::size_t& id) : id_(id) {}
    template<typename Task>
    void operator()(Task* task) const {
        task->node_.id = id_++;
    }
    std::size_t& id_;
};

struct schedule_visitor {
    template<typename Task>
    void operator()(Task* task) const {
        if (!task->future_.valid()) {
            auto self = task->shared_from_this();
            if (task->pool_) {
                task->future_ = task->pool_->push(&Task::evaluate, self);
            } else {
                auto pkg = std::packaged_task<typename Task::result_type()>(std::bind(&Task::evaluate, self));
                task->future_ = pkg.get_future();
                pkg();
            }
        }
    }
};

struct wait_visitor {
    template<typename Task>
    void operator()(Task* task) const {
        if (task->future_.valid())
            task->future_.wait();
    }
};

struct reset_future_visitor {
    template<typename Task>
    void operator()(Task* task) const {
        task->future_ = std::shared_future<typename Task::result_type>();
    }
};

struct graph_visitor {
    explicit graph_visitor(std::vector<transwarp::edge>& graph) : graph_(graph) {}
    template<typename Task>
    void operator()(Task* task) const {
        transwarp::detail::apply(transwarp::detail::make_edges_functor(graph_, task->node_), task->tasks_);
    }
    std::vector<transwarp::edge>& graph_;
};

struct set_pool_visitor {
    explicit set_pool_visitor(std::shared_ptr<cxxpool::thread_pool> pool)
    : pool_(std::move(pool))
    {}
    template<typename Task>
    void operator()(Task* task) const {
        task->pool_ = pool_;
    }
    std::shared_ptr<cxxpool::thread_pool> pool_;
};

struct reset_pool_visitor {
    template<typename Task>
    void operator()(Task* task) const {
        task->pool_.reset();
    }
};

} // detail


struct pass_visitor {
    template<typename Task>
    void operator()(Task* task) const {}
};


inline
std::string make_dot_graph(const std::vector<transwarp::edge>& graph, const std::string& name="transwarp") {
    auto info = [](transwarp::node n) {
        auto name = transwarp::detail::trim(n.name);
        std::replace(name.begin(), name.end(), ' ', '\n');
        return '"' + std::to_string(n.id) + "\n" + name + '"';
    };
    std::string dot = "digraph " + name + " {\n";
    for (const auto& pair : graph) {
        dot += info(pair.parent) + " -> " + info(pair.child) + '\n';
    }
    dot += "}\n";
    return dot;
}


template<typename Functor, typename... Tasks>
class task : public std::enable_shared_from_this<transwarp::task<Functor, Tasks...>> {
public:
    using result_type = typename std::result_of<Functor(typename Tasks::result_type...)>::type;

    task(std::string name, Functor functor, std::shared_ptr<Tasks>... tasks)
    : node_{0, std::move(name)},
      functor_(std::move(functor)),
      tasks_(std::make_tuple(std::move(tasks)...)),
      visited_(false)
    {}

    void finalize() {
        std::size_t id = 0;
        transwarp::pass_visitor pass;
        transwarp::detail::id_visitor post_visitor(id);
        visit(pass, post_visitor);
        unvisit();
    }

    const transwarp::node& get_node() const {
        return node_;
    }

    const Functor& get_functor() const {
        return functor_;
    }

    const std::tuple<std::shared_ptr<Tasks>...>& get_tasks() const {
        return tasks_;
    }

    template<typename PreVisitor, typename PostVisitor>
    void visit(PreVisitor& pre_visitor, PostVisitor& post_visitor) {
        if (!visited_) {
            pre_visitor(this);
            transwarp::detail::apply(transwarp::detail::visit_functor<PreVisitor, PostVisitor>(pre_visitor, post_visitor), tasks_);
            post_visitor(this);
            visited_ = true;
        }
    }

    void unvisit() {
        if (visited_) {
            visited_ = false;
            transwarp::detail::apply(transwarp::detail::unvisit_functor(), tasks_);
        }
    }

    void set_parallel(std::size_t n_threads, std::function<void(std::thread&)> thread_prioritizer=nullptr) {
        transwarp::pass_visitor pass;
        if (n_threads > 0) {
            auto pool = std::make_shared<cxxpool::thread_pool>(n_threads);
            if (thread_prioritizer)
                pool->set_thread_prioritizer(std::move(thread_prioritizer));
            transwarp::detail::set_pool_visitor pre_visitor(std::move(pool));
            visit(pre_visitor, pass);
        } else {
            transwarp::detail::reset_pool_visitor pre_visitor;
            visit(pre_visitor, pass);
        }
        unvisit();
    }

    void schedule() {
        transwarp::pass_visitor pass;
        transwarp::detail::schedule_visitor post_visitor;
        visit(pass, post_visitor);
        unvisit();
    }

    std::shared_future<transwarp::task<Functor, Tasks...>::result_type> get_future() const {
        return future_;
    }

    void wait() {
        transwarp::pass_visitor pass;
        transwarp::detail::wait_visitor post_visitor;
        visit(pass, post_visitor);
        unvisit();
    }

    void reset() {
        transwarp::pass_visitor pass;
        transwarp::detail::reset_future_visitor pre_visitor;
        visit(pre_visitor, pass);
        unvisit();
    }

    std::vector<transwarp::edge> get_graph() {
        std::vector<transwarp::edge> graph;
        transwarp::pass_visitor pass;
        transwarp::detail::graph_visitor pre_visitor(graph);
        visit(pre_visitor, pass);
        unvisit();
        return graph;
    }

private:

    friend struct transwarp::detail::reset_pool_visitor;
    friend struct transwarp::detail::set_pool_visitor;
    friend struct transwarp::detail::graph_visitor;
    friend struct transwarp::detail::reset_future_visitor;
    friend struct transwarp::detail::wait_visitor;
    friend struct transwarp::detail::schedule_visitor;
    friend struct transwarp::detail::id_visitor;

    static transwarp::task<Functor, Tasks...>::result_type evaluate(std::shared_ptr<transwarp::task<Functor, Tasks...>> task) {
        return transwarp::detail::call<transwarp::task<Functor, Tasks...>::result_type>(task->functor_, task->tasks_);
    }

    transwarp::node node_;
    Functor functor_;
    std::tuple<std::shared_ptr<Tasks>...> tasks_;
    bool visited_;
    std::shared_ptr<cxxpool::thread_pool> pool_;
    std::shared_future<transwarp::task<Functor, Tasks...>::result_type> future_;
};


template<typename Functor, typename... Tasks>
std::shared_ptr<transwarp::task<Functor, Tasks...>> make_task(std::string name, Functor functor, std::shared_ptr<Tasks>... tasks) {
    return std::make_shared<transwarp::task<Functor, Tasks...>>(std::move(name), std::move(functor), std::move(tasks)...);
}


} // transwarp