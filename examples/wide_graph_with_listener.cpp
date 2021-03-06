#include "wide_graph_with_listener.h"
#include "../src/transwarp.h"
#include <iostream>
#include <fstream>
#include <random>
#include <numeric>
namespace tw = transwarp;

namespace {

std::mt19937 gen{1};
std::mutex mutex; // protect the generator

using data_t = std::shared_ptr<std::vector<double>>;

data_t transform(data_t data) {
    std::uniform_real_distribution<double> dist(0.5, 1.5);
    for (auto& v : *data) {
        std::lock_guard<std::mutex> lock{mutex};
        v *= dist(gen);
    }
    return data;
}

double mean(data_t data) {
    return std::accumulate(data->begin(), data->end(), 0.) / static_cast<double>(data->size());
}

class listener : public tw::listener {
public:

    // Note: this is called on the thread the task is run on for the after_finished event
    void handle_event(tw::event_type event, const std::shared_ptr<tw::node>&) {
        if (event == tw::event_type::after_finished) {
            // task has finished
        }
    }

};

std::shared_ptr<tw::task<double>> build_graph(std::shared_ptr<tw::task<data_t>> input) {
    auto d0 = tw::make_task(tw::consume, transform, input)->then(tw::consume, transform);
    auto d1 = tw::make_task(tw::consume, transform, input)->then(tw::consume, transform);
    auto d2 = tw::make_task(tw::consume, transform, input)->then(tw::consume, transform);
    auto d3 = tw::make_task(tw::consume, transform, input)->then(tw::consume, transform);
    auto d4 = tw::make_task(tw::consume, transform, input)->then(tw::consume, transform);
    auto d5 = tw::make_task(tw::consume, transform, input)->then(tw::consume, transform);
    auto d6 = tw::make_task(tw::consume, transform, input)->then(tw::consume, transform);
    auto d7 = tw::make_task(tw::consume, transform, input)->then(tw::consume, transform);
    auto final = tw::make_task(tw::consume,
        [](data_t d0, data_t d1, data_t d2, data_t d3, data_t d4, data_t d5, data_t d6, data_t d7) {
            return (mean(d0) + mean(d1) + mean(d2) + mean(d3) + mean(d4) + mean(d5) + mean(d6) + mean(d7)) / 8.;
        }, d0, d1, d2, d3, d4, d5, d6, d7);
    return final;
}

}


namespace examples {

// This example demonstrates the scheduling of an extra wide graph.
// It also shows how the listener interface can be used to handle task events.
// Increase iterations and size and observe your CPU load.
// In this example, new data cannot be scheduled until the last result
// was retrieved. However, this can be changed to use a pool of graphs
// to process every data input as soon as possible.
void wide_graph_with_listener(std::ostream& os, std::size_t iterations, std::size_t size) {
    tw::parallel exec{8}; // thread pool with 8 threads

    // The data input task at the root of the graph
    auto input = tw::make_value_task(std::make_shared<std::vector<double>>());

    // Build graph and return the final task
    auto final = build_graph(input);
    final->add_listener(std::make_shared<listener>());

    // Output the graph for visualization
    const auto graph = final->get_graph();
    std::ofstream("wide_graph_with_listener.dot") << tw::to_string(graph);

    // This is to generate random data
    std::uniform_int_distribution<std::size_t> dist(size, size * 10);

    for (std::size_t i=0; i<iterations; ++i) {
        auto data = std::make_shared<std::vector<double>>(dist(gen), 1);
        input->set_value(data); // New data arrive
        final->schedule_all(exec); // Schedule the graph immediately after data arrived
        os << final->get() << std::endl; // Print result
    }
}

}


#ifndef UNITTEST
int main() {
    std::cout << "Running example: wide_graph_with_listener ..." << std::endl;
    examples::wide_graph_with_listener(std::cout);
}
#endif
