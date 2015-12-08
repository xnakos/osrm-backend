/*

Copyright (c) 2015, Project OSRM contributors
All rights reserved.

Redistribution and use in source and binary forms, with or without modification,
are permitted provided that the following conditions are met:

Redistributions of source code must retain the above copyright notice, this list
of conditions and the following disclaimer.
Redistributions in binary form must reproduce the above copyright notice, this
list of conditions and the following disclaimer in the documentation and/or
other materials provided with the distribution.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR
ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

*/

#include "extractor.hpp"

#include "extraction_containers.hpp"
#include "extraction_node.hpp"
#include "extraction_way.hpp"
#include "extractor_callbacks.hpp"
#include "restriction_parser.hpp"
#include "scripting_environment.hpp"

#include "../data_structures/raster_source.hpp"
#include "../util/make_unique.hpp"
#include "../util/simple_logger.hpp"
#include "../util/timing_util.hpp"
#include "../util/lua_util.hpp"
#include "../util/graph_loader.hpp"

#include "../typedefs.h"

#include "../data_structures/static_graph.hpp"
#include "../data_structures/static_rtree.hpp"
#include "../data_structures/restriction_map.hpp"
#include "../data_structures/compressed_edge_container.hpp"

#include "../algorithms/tarjan_scc.hpp"
#include "../algorithms/crc32_processor.hpp"

#include <boost/filesystem.hpp>
#include <boost/filesystem/fstream.hpp>
#include <boost/optional/optional.hpp>

#include <luabind/luabind.hpp>

#include <osmium/io/any_input.hpp>

#include <tbb/parallel_for.h>
#include <tbb/task_scheduler_init.h>

#include <cstdlib>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <fstream>
#include <iostream>
#include <thread>
#include <unordered_map>
#include <vector>

/**
 * TODO: Refactor this function into smaller functions for better readability.
 *
 * This function is the entry point for the whole extraction process. The goal of the extraction
 * step is to filter and convert the OSM geometry to something more fitting for routing.
 * That includes:
 *  - extracting turn restrictions
 *  - splitting ways into (directional!) edge segments
 *  - checking if nodes are barriers or traffic signal
 *  - discarding all tag information: All relevant type information for nodes/ways
 *    is extracted at this point.
 *
 * The result of this process are the following files:
 *  .names : Names of all streets, stored as long consecutive string with prefix sum based index
 *  .osrm  : Nodes and edges in a intermediate format that easy to digest for osrm-prepare
 *  .restrictions : Turn restrictions that are used my osrm-prepare to construct the edge-expanded graph
 *
 */
int extractor::run()
{
    try
    {
        LogPolicy::GetInstance().Unmute();
        TIMER_START(extracting);

        const unsigned recommended_num_threads = tbb::task_scheduler_init::default_num_threads();
        const auto number_of_threads =
            std::min(recommended_num_threads, config.requested_num_threads);
        tbb::task_scheduler_init init(number_of_threads);

        SimpleLogger().Write() << "Input file: " << config.input_path.filename().string();
        SimpleLogger().Write() << "Profile: " << config.profile_path.filename().string();
        SimpleLogger().Write() << "Threads: " << number_of_threads;

        // setup scripting environment
        ScriptingEnvironment scripting_environment(config.profile_path.string().c_str());

        ExtractionContainers extraction_containers;
        auto extractor_callbacks = osrm::make_unique<ExtractorCallbacks>(extraction_containers);

        const osmium::io::File input_file(config.input_path.string());
        osmium::io::Reader reader(input_file);
        const osmium::io::Header header = reader.header();

        std::atomic<unsigned> number_of_nodes{0};
        std::atomic<unsigned> number_of_ways{0};
        std::atomic<unsigned> number_of_relations{0};
        std::atomic<unsigned> number_of_others{0};

        SimpleLogger().Write() << "Parsing in progress..";
        TIMER_START(parsing);

        lua_State *segment_state = scripting_environment.get_lua_state();

        if (lua_function_exists(segment_state, "source_function"))
        {
            // bind a single instance of SourceContainer class to relevant lua state
            SourceContainer sources;
            luabind::globals(segment_state)["sources"] = sources;

            luabind::call_function<void>(segment_state, "source_function");
        }

        std::string generator = header.get("generator");
        if (generator.empty())
        {
            generator = "unknown tool";
        }
        SimpleLogger().Write() << "input file generated by " << generator;

        // write .timestamp data file
        std::string timestamp = header.get("osmosis_replication_timestamp");
        if (timestamp.empty())
        {
            timestamp = "n/a";
        }
        SimpleLogger().Write() << "timestamp: " << timestamp;

        boost::filesystem::ofstream timestamp_out(config.timestamp_file_name);
        timestamp_out.write(timestamp.c_str(), timestamp.length());
        timestamp_out.close();

        // initialize vectors holding parsed objects
        tbb::concurrent_vector<std::pair<std::size_t, ExtractionNode>> resulting_nodes;
        tbb::concurrent_vector<std::pair<std::size_t, ExtractionWay>> resulting_ways;
        tbb::concurrent_vector<boost::optional<InputRestrictionContainer>> resulting_restrictions;

        // setup restriction parser
        const RestrictionParser restriction_parser(scripting_environment.get_lua_state());

        while (const osmium::memory::Buffer buffer = reader.read())
        {
            // create a vector of iterators into the buffer
            std::vector<osmium::memory::Buffer::const_iterator> osm_elements;
            for (auto iter = std::begin(buffer), end = std::end(buffer); iter != end; ++iter)
            {
                osm_elements.push_back(iter);
            }

            // clear resulting vectors
            resulting_nodes.clear();
            resulting_ways.clear();
            resulting_restrictions.clear();

            // parse OSM entities in parallel, store in resulting vectors
            tbb::parallel_for(
                tbb::blocked_range<std::size_t>(0, osm_elements.size()),
                [&](const tbb::blocked_range<std::size_t> &range)
                {
                    ExtractionNode result_node;
                    ExtractionWay result_way;
                    lua_State *local_state = scripting_environment.get_lua_state();

                    for (auto x = range.begin(), end = range.end(); x != end; ++x)
                    {
                        const auto entity = osm_elements[x];

                        switch (entity->type())
                        {
                        case osmium::item_type::node:
                            result_node.clear();
                            ++number_of_nodes;
                            luabind::call_function<void>(
                                local_state, "node_function",
                                boost::cref(static_cast<const osmium::Node &>(*entity)),
                                boost::ref(result_node));
                            resulting_nodes.push_back(std::make_pair(x, result_node));
                            break;
                        case osmium::item_type::way:
                            result_way.clear();
                            ++number_of_ways;
                            luabind::call_function<void>(
                                local_state, "way_function",
                                boost::cref(static_cast<const osmium::Way &>(*entity)),
                                boost::ref(result_way));
                            resulting_ways.push_back(std::make_pair(x, result_way));
                            break;
                        case osmium::item_type::relation:
                            ++number_of_relations;
                            resulting_restrictions.push_back(restriction_parser.TryParse(
                                static_cast<const osmium::Relation &>(*entity)));
                            break;
                        default:
                            ++number_of_others;
                            break;
                        }
                    }
                });

            // put parsed objects thru extractor callbacks
            for (const auto &result : resulting_nodes)
            {
                extractor_callbacks->ProcessNode(
                    static_cast<const osmium::Node &>(*(osm_elements[result.first])),
                    result.second);
            }
            for (const auto &result : resulting_ways)
            {
                extractor_callbacks->ProcessWay(
                    static_cast<const osmium::Way &>(*(osm_elements[result.first])), result.second);
            }
            for (const auto &result : resulting_restrictions)
            {
                extractor_callbacks->ProcessRestriction(result);
            }
        }
        TIMER_STOP(parsing);
        SimpleLogger().Write() << "Parsing finished after " << TIMER_SEC(parsing) << " seconds";

        SimpleLogger().Write() << "Raw input contains " << number_of_nodes.load() << " nodes, "
                               << number_of_ways.load() << " ways, and "
                               << number_of_relations.load() << " relations, and "
                               << number_of_others.load() << " unknown entities";

        extractor_callbacks.reset();

        if (extraction_containers.all_edges_list.empty())
        {
            SimpleLogger().Write(logWARNING) << "The input data is empty, exiting.";
            return 1;
        }

        extraction_containers.PrepareData(config.output_file_name,
                                          config.restriction_file_name,
                                          config.names_file_name,
                                          segment_state);

        TIMER_STOP(extracting);
        SimpleLogger().Write() << "extraction finished after " << TIMER_SEC(extracting) << "s";
    }
    catch (const std::exception &e)
    {
        SimpleLogger().Write(logWARNING) << e.what();
        return 1;
    }

    try
    {
        // Transform the node-based graph that OSM is based on into an edge-based graph
        // that is better for routing.  Every edge becomes a node, and every valid
        // movement (e.g. turn from A->B, and B->A) becomes an edge
        //
        //
        //    // Create a new lua state

        SimpleLogger().Write() << "Generating edge-expanded graph representation";

        TIMER_START(expansion);

        std::vector<EdgeBasedNode> node_based_edge_list;
        DeallocatingVector<EdgeBasedEdge> edge_based_edge_list;
        std::vector<QueryNode> internal_to_external_node_map;
        auto graph_size =
            BuildEdgeExpandedGraph(internal_to_external_node_map,
                                   node_based_edge_list, 
                                   edge_based_edge_list);

        auto number_of_node_based_nodes = graph_size.first;
        auto max_edge_id = graph_size.second;

        TIMER_STOP(expansion);

        SimpleLogger().Write() << "building r-tree ...";
        TIMER_START(rtree);

        FindComponents(max_edge_id, edge_based_edge_list, node_based_edge_list);

        BuildRTree(node_based_edge_list, internal_to_external_node_map);

        TIMER_STOP(rtree);

        SimpleLogger().Write() << "writing node map ...";
        WriteNodeMapping(internal_to_external_node_map);

        WriteEdgeBasedGraph(config.edge_graph_output_path, max_edge_id, edge_based_edge_list);

        SimpleLogger().Write() << "Expansion  : " << (number_of_node_based_nodes / TIMER_SEC(expansion))
                        << " nodes/sec and " << ((max_edge_id + 1) / TIMER_SEC(expansion))
                        << " edges/sec";
        SimpleLogger().Write() << "To prepare the data for routing, run: "
                               << "./osrm-prepare " << config.output_file_name
                               << std::endl;
    }
    catch (const std::exception &e)
    {
        SimpleLogger().Write(logWARNING) << e.what();
        return 1;
    }

    return 0;
}

/**
    \brief Setups scripting environment (lua-scripting)
    Also initializes speed profile.
*/
void extractor::SetupScriptingEnvironment(lua_State *lua_state, SpeedProfileProperties &speed_profile)
{
    // open utility libraries string library;
    luaL_openlibs(lua_state);

    // adjust lua load path
    luaAddScriptFolderToLoadPath(lua_state, config.profile_path.string().c_str());

    // Now call our function in a lua script
    if (0 != luaL_dofile(lua_state, config.profile_path.string().c_str()))
    {
        std::stringstream msg;
        msg << lua_tostring(lua_state, -1) << " occured in scripting block";
        throw osrm::exception(msg.str());
    }

    if (0 != luaL_dostring(lua_state, "return traffic_signal_penalty\n"))
    {
        std::stringstream msg;
        msg << lua_tostring(lua_state, -1) << " occured in scripting block";
        throw osrm::exception(msg.str());
    }
    speed_profile.traffic_signal_penalty = 10 * lua_tointeger(lua_state, -1);
    SimpleLogger().Write(logDEBUG)
        << "traffic_signal_penalty: " << speed_profile.traffic_signal_penalty;

    if (0 != luaL_dostring(lua_state, "return u_turn_penalty\n"))
    {
        std::stringstream msg;
        msg << lua_tostring(lua_state, -1) << " occured in scripting block";
        throw osrm::exception(msg.str());
    }

    speed_profile.u_turn_penalty = 10 * lua_tointeger(lua_state, -1);
    speed_profile.has_turn_penalty_function = lua_function_exists(lua_state, "turn_function");
}

void extractor::FindComponents(unsigned max_edge_id,
                               const DeallocatingVector<EdgeBasedEdge> &input_edge_list,
                               std::vector<EdgeBasedNode> &input_nodes) const
{
    struct UncontractedEdgeData
    {
    };
    struct InputEdge
    {
        unsigned source;
        unsigned target;
        UncontractedEdgeData data;

        bool operator<(const InputEdge &rhs) const
        {
            return source < rhs.source || (source == rhs.source && target < rhs.target);
        }

        bool operator==(const InputEdge &rhs) const
        {
            return source == rhs.source && target == rhs.target;
        }
    };
    using UncontractedGraph = StaticGraph<UncontractedEdgeData>;
    std::vector<InputEdge> edges;
    edges.reserve(input_edge_list.size() * 2);

    for (const auto &edge : input_edge_list)
    {
        BOOST_ASSERT_MSG(static_cast<unsigned int>(std::max(edge.weight, 1)) > 0,
                         "edge distance < 1");
        if (edge.forward)
        {
            edges.push_back({edge.source, edge.target, {}});
        }

        if (edge.backward)
        {
            edges.push_back({edge.target, edge.source, {}});
        }
    }

    // connect forward and backward nodes of each edge
    for (const auto &node : input_nodes)
    {
        if (node.reverse_edge_based_node_id != SPECIAL_NODEID)
        {
            edges.push_back({node.forward_edge_based_node_id, node.reverse_edge_based_node_id, {}});
            edges.push_back({node.reverse_edge_based_node_id, node.forward_edge_based_node_id, {}});
        }
    }

    tbb::parallel_sort(edges.begin(), edges.end());
    auto new_end = std::unique(edges.begin(), edges.end());
    edges.resize(new_end - edges.begin());

    auto uncontractor_graph = std::make_shared<UncontractedGraph>(max_edge_id + 1, edges);

    TarjanSCC<UncontractedGraph> component_search(
        std::const_pointer_cast<const UncontractedGraph>(uncontractor_graph));
    component_search.run();

    for (auto &node : input_nodes)
    {
        auto forward_component = component_search.get_component_id(node.forward_edge_based_node_id);
        BOOST_ASSERT(node.reverse_edge_based_node_id == SPECIAL_EDGEID ||
                     forward_component ==
                         component_search.get_component_id(node.reverse_edge_based_node_id));

        const unsigned component_size = component_search.get_component_size(forward_component);
        node.component.is_tiny = component_size < 1000;
        node.component.id = 1 + forward_component;
    }
}

/**
  \brief Build load restrictions from .restriction file
  */
std::shared_ptr<RestrictionMap> extractor::LoadRestrictionMap()
{
    boost::filesystem::ifstream input_stream(config.restriction_file_name,
                                             std::ios::in | std::ios::binary);

    std::vector<TurnRestriction> restriction_list;
    loadRestrictionsFromFile(input_stream, restriction_list);

    SimpleLogger().Write() << " - " << restriction_list.size() << " restrictions.";

    return std::make_shared<RestrictionMap>(restriction_list);
}

/**
  \brief Load node based graph from .osrm file
  */
std::shared_ptr<NodeBasedDynamicGraph>
extractor::LoadNodeBasedGraph(std::unordered_set<NodeID> &barrier_nodes,
                            std::unordered_set<NodeID> &traffic_lights,
                            std::vector<QueryNode> &internal_to_external_node_map)
{
    std::vector<NodeBasedEdge> edge_list;

    boost::filesystem::ifstream input_stream(config.output_file_name,
                                             std::ios::in | std::ios::binary);

    std::vector<NodeID> barrier_list;
    std::vector<NodeID> traffic_light_list;
    NodeID number_of_node_based_nodes = loadNodesFromFile(
        input_stream, barrier_list, traffic_light_list, internal_to_external_node_map);

    SimpleLogger().Write() << " - " << barrier_list.size() << " bollard nodes, "
                           << traffic_light_list.size() << " traffic lights";

    // insert into unordered sets for fast lookup
    barrier_nodes.insert(barrier_list.begin(), barrier_list.end());
    traffic_lights.insert(traffic_light_list.begin(), traffic_light_list.end());

    barrier_list.clear();
    barrier_list.shrink_to_fit();
    traffic_light_list.clear();
    traffic_light_list.shrink_to_fit();

    loadEdgesFromFile(input_stream, edge_list);

    if (edge_list.empty())
    {
        SimpleLogger().Write(logWARNING) << "The input data is empty, exiting.";
        return std::shared_ptr<NodeBasedDynamicGraph>();
    }

    return NodeBasedDynamicGraphFromEdges(number_of_node_based_nodes, edge_list);
}

/**
 \brief Building an edge-expanded graph from node-based input and turn restrictions
*/
std::pair<std::size_t, std::size_t>
extractor::BuildEdgeExpandedGraph(std::vector<QueryNode> &internal_to_external_node_map,
                                std::vector<EdgeBasedNode> &node_based_edge_list,
                                DeallocatingVector<EdgeBasedEdge> &edge_based_edge_list)
{
    lua_State *lua_state = luaL_newstate();
    luabind::open(lua_state);

    SpeedProfileProperties speed_profile;
    SetupScriptingEnvironment(lua_state, speed_profile);

    std::unordered_set<NodeID> barrier_nodes;
    std::unordered_set<NodeID> traffic_lights;

    auto restriction_map = LoadRestrictionMap();
    auto node_based_graph =
        LoadNodeBasedGraph(barrier_nodes, traffic_lights, internal_to_external_node_map);

    CompressedEdgeContainer compressed_edge_container;
    GraphCompressor graph_compressor(speed_profile);
    graph_compressor.Compress(barrier_nodes, traffic_lights, *restriction_map, *node_based_graph,
                              compressed_edge_container);

    EdgeBasedGraphFactory edge_based_graph_factory(
        node_based_graph, compressed_edge_container, barrier_nodes, traffic_lights,
        std::const_pointer_cast<RestrictionMap const>(restriction_map),
        internal_to_external_node_map, speed_profile);


    compressed_edge_container.SerializeInternalVector(config.geometry_output_path);

    edge_based_graph_factory.Run(config.edge_output_path, lua_state,
            config.edge_segment_lookup_path,
            config.edge_penalty_path,
            config.generate_edge_lookup
#ifdef DEBUG_GEOMETRY
            , config.debug_turns_path
#endif
            );
    lua_close(lua_state);

    edge_based_graph_factory.GetEdgeBasedEdges(edge_based_edge_list);
    edge_based_graph_factory.GetEdgeBasedNodes(node_based_edge_list);
    auto max_edge_id = edge_based_graph_factory.GetHighestEdgeID();

    const std::size_t number_of_node_based_nodes = node_based_graph->GetNumberOfNodes();
    return std::make_pair(number_of_node_based_nodes, max_edge_id);
}


/**
  \brief Writing info on original (node-based) nodes
 */
void extractor::WriteNodeMapping(const std::vector<QueryNode> & internal_to_external_node_map)
{
    boost::filesystem::ofstream node_stream(config.node_output_path, std::ios::binary);
    const unsigned size_of_mapping = internal_to_external_node_map.size();
    node_stream.write((char *)&size_of_mapping, sizeof(unsigned));
    if (size_of_mapping > 0)
    {
        node_stream.write((char *)internal_to_external_node_map.data(),
                          size_of_mapping * sizeof(QueryNode));
    }
    node_stream.close();
}

/**
    \brief Building rtree-based nearest-neighbor data structure

    Saves tree into '.ramIndex' and leaves into '.fileIndex'.
 */
void extractor::BuildRTree(const std::vector<EdgeBasedNode> &node_based_edge_list,
                         const std::vector<QueryNode> &internal_to_external_node_map)
{
    StaticRTree<EdgeBasedNode>(node_based_edge_list, config.rtree_nodes_output_path.c_str(),
                               config.rtree_leafs_output_path.c_str(),
                               internal_to_external_node_map);
}

void extractor::WriteEdgeBasedGraph(std::string const &output_file_filename, 
                                    size_t const max_edge_id, 
                                    DeallocatingVector<EdgeBasedEdge> const & edge_based_edge_list) 
{

    std::ofstream file_out_stream;
    file_out_stream.open(output_file_filename.c_str(), std::ios::binary);
    const FingerPrint fingerprint = FingerPrint::GetValid();
    file_out_stream.write((char *)&fingerprint, sizeof(FingerPrint));

    std::ofstream myEdgeBasedEdgesTxtFile;
    myEdgeBasedEdgesTxtFile.open("myEdgeBasedEdges.txt");

    myEdgeBasedEdgesTxtFile << "source\ttarget\tedge_id\tweight\tforward\tbackward" << std::endl;

    std::cout << "[extractor] Writing edge-based-graph egdes       ... " << std::flush;
    TIMER_START(write_edges);

    size_t number_of_used_edges = edge_based_edge_list.size();
    file_out_stream.write((char *)&number_of_used_edges, sizeof(size_t));
    file_out_stream.write((char *)&max_edge_id, sizeof(size_t));

    for (const auto& edge : edge_based_edge_list) {
        file_out_stream.write((char *) &edge, sizeof(EdgeBasedEdge));
        myEdgeBasedEdgesTxtFile << edge.source << "\t" << edge.target << "\t" << edge.edge_id << "\t" << edge.weight << "\t" << edge.forward << "\t" << edge.backward << std::endl;
    }

    TIMER_STOP(write_edges);
    std::cout << "ok, after " << TIMER_SEC(write_edges) << "s" << std::endl;

    myEdgeBasedEdgesTxtFile.close();

    SimpleLogger().Write() << "Processed " << number_of_used_edges << " edges";
    file_out_stream.close();

}
