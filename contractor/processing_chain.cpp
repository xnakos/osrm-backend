/*

Copyright (c) 2015, Project OSRM, Dennis Luxen, others
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

#include "processing_chain.hpp"
#include "contractor.hpp"

#include "contractor.hpp"

#include "../data_structures/deallocating_vector.hpp"

#include "../algorithms/crc32_processor.hpp"
#include "../util/graph_loader.hpp"
#include "../util/integer_range.hpp"
#include "../util/lua_util.hpp"
#include "../util/osrm_exception.hpp"
#include "../util/simple_logger.hpp"
#include "../util/string_util.hpp"
#include "../util/timing_util.hpp"
#include "../typedefs.h"

#include <fast-cpp-csv-parser/csv.h>

#include <boost/filesystem/fstream.hpp>
#include <boost/program_options.hpp>

#include <tbb/parallel_sort.h>

#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "../util/debug_geometry.hpp"

Prepare::~Prepare() {}

int Prepare::Run()
{
#ifdef WIN32
#pragma message("Memory consumption on Windows can be higher due to different bit packing")
#else
    static_assert(sizeof(NodeBasedEdge) == 20,
                  "changing NodeBasedEdge type has influence on memory consumption!");
    static_assert(sizeof(EdgeBasedEdge) == 16,
                  "changing EdgeBasedEdge type has influence on memory consumption!");
#endif

    if (config.core_factor > 1.0 || config.core_factor < 0) 
    {
       throw osrm::exception("Core factor must be between 0.0 to 1.0 (inclusive)");
    }

    TIMER_START(preparing);

    // Create a new lua state

    SimpleLogger().Write() << "Loading edge-expanded graph representation";

    DeallocatingVector<EdgeBasedEdge> edge_based_edge_list;

    size_t max_edge_id = LoadEdgeExpandedGraph(
        config.edge_based_graph_path, edge_based_edge_list, config.edge_segment_lookup_path,
        config.edge_penalty_path, config.segment_speed_lookup_path);

    // Contracting the edge-expanded graph

    TIMER_START(contraction);
    std::vector<bool> is_core_node;
    std::vector<float> node_levels;
    if (config.use_cached_priority)
    {
        ReadNodeLevels(node_levels);
    }
    DeallocatingVector<QueryEdge> contracted_edge_list;
    ContractGraph(max_edge_id, edge_based_edge_list, contracted_edge_list, is_core_node,
                  node_levels);
    TIMER_STOP(contraction);

    SimpleLogger().Write() << "Contraction took " << TIMER_SEC(contraction) << " sec";

    std::size_t number_of_used_edges = WriteContractedGraph(max_edge_id, contracted_edge_list);
    WriteCoreNodeMarker(std::move(is_core_node));
    if (!config.use_cached_priority)
    {
        WriteNodeLevels(std::move(node_levels));
    }

    TIMER_STOP(preparing);

    SimpleLogger().Write() << "Preprocessing : " << TIMER_SEC(preparing) << " seconds";
    SimpleLogger().Write() << "Contraction: " << ((max_edge_id + 1) / TIMER_SEC(contraction))
                           << " nodes/sec and " << number_of_used_edges / TIMER_SEC(contraction)
                           << " edges/sec";

    SimpleLogger().Write() << "finished preprocessing";

    return 0;
}

namespace std
{

template <> struct hash<std::pair<OSMNodeID, OSMNodeID>>
{
    std::size_t operator()(const std::pair<OSMNodeID, OSMNodeID> &k) const
    {
        return OSMNodeID_to_uint64_t(k.first) ^ (OSMNodeID_to_uint64_t(k.second) << 12);
    }
};
}

std::size_t Prepare::LoadEdgeExpandedGraph(std::string const &edge_based_graph_filename,
                                           DeallocatingVector<EdgeBasedEdge> &edge_based_edge_list,
                                           const std::string &edge_segment_lookup_filename,
                                           const std::string &edge_penalty_filename,
                                           const std::string &segment_speed_filename)
{
    SimpleLogger().Write() << "Opening " << edge_based_graph_filename;
    boost::filesystem::ifstream input_stream(edge_based_graph_filename, std::ios::binary);

    const bool update_edge_weights = segment_speed_filename != "";

    boost::filesystem::ifstream edge_segment_input_stream;
    boost::filesystem::ifstream edge_fixed_penalties_input_stream;

    if (update_edge_weights)
    {
        edge_segment_input_stream.open(edge_segment_lookup_filename, std::ios::binary);
        edge_fixed_penalties_input_stream.open(edge_penalty_filename, std::ios::binary);
        if (!edge_segment_input_stream || !edge_fixed_penalties_input_stream)
        {
            throw osrm::exception("Could not load .edge_segment_lookup or .edge_penalties, did you "
                                  "run osrm-extract with '--generate-edge-lookup'?");
        }
    }

    const FingerPrint fingerprint_valid = FingerPrint::GetValid();
    FingerPrint fingerprint_loaded;
    input_stream.read((char *)&fingerprint_loaded, sizeof(FingerPrint));
    fingerprint_loaded.TestPrepare(fingerprint_valid);

    size_t number_of_edges = 0;
    size_t max_edge_id = SPECIAL_EDGEID;
    input_stream.read((char *)&number_of_edges, sizeof(size_t));
    input_stream.read((char *)&max_edge_id, sizeof(size_t));

    edge_based_edge_list.resize(number_of_edges);
    SimpleLogger().Write() << "Reading " << number_of_edges << " edges from the edge based graph";

    std::unordered_map<std::pair<OSMNodeID, OSMNodeID>, unsigned> segment_speed_lookup;

    if (update_edge_weights)
    {
        SimpleLogger().Write() << "Segment speed data supplied, will update edge weights from "
                               << segment_speed_filename;
        io::CSVReader<3> csv_in(segment_speed_filename);
        csv_in.set_header("from_node", "to_node", "speed");
        uint64_t from_node_id;
        uint64_t to_node_id;
        unsigned speed;
        while (csv_in.read_row(from_node_id, to_node_id, speed))
        {
            segment_speed_lookup[std::make_pair(OSMNodeID(from_node_id), OSMNodeID(to_node_id))] = speed;
        }
    }

    DEBUG_GEOMETRY_START(config);

    // TODO: can we read this in bulk?  DeallocatingVector isn't necessarily
    // all stored contiguously
    for (; number_of_edges > 0; --number_of_edges)
    {
        EdgeBasedEdge inbuffer;
        input_stream.read((char *) &inbuffer, sizeof(EdgeBasedEdge));

        if (update_edge_weights)
        {
            // Processing-time edge updates
            unsigned fixed_penalty;
            edge_fixed_penalties_input_stream.read(reinterpret_cast<char *>(&fixed_penalty),
                                                   sizeof(fixed_penalty));

            int new_weight = 0;

            unsigned num_osm_nodes = 0;
            edge_segment_input_stream.read(reinterpret_cast<char *>(&num_osm_nodes),
                                           sizeof(num_osm_nodes));
            OSMNodeID previous_osm_node_id;
            edge_segment_input_stream.read(reinterpret_cast<char *>(&previous_osm_node_id),
                                           sizeof(previous_osm_node_id));
            OSMNodeID this_osm_node_id;
            double segment_length;
            int segment_weight;
            --num_osm_nodes;
            for (; num_osm_nodes != 0; --num_osm_nodes)
            {
                edge_segment_input_stream.read(reinterpret_cast<char *>(&this_osm_node_id),
                                               sizeof(this_osm_node_id));
                edge_segment_input_stream.read(reinterpret_cast<char *>(&segment_length),
                                               sizeof(segment_length));
                edge_segment_input_stream.read(reinterpret_cast<char *>(&segment_weight),
                                               sizeof(segment_weight));

                auto speed_iter = segment_speed_lookup.find(
                    std::make_pair(previous_osm_node_id, this_osm_node_id));
                if (speed_iter != segment_speed_lookup.end())
                {
                    // This sets the segment weight using the same formula as the
                    // EdgeBasedGraphFactory for consistency.  The *why* of this formula
                    // is lost in the annals of time.
                    int new_segment_weight =
                        std::max(1, static_cast<int>(std::floor(
                                        (segment_length * 10.) / (speed_iter->second / 3.6) + .5)));
                    new_weight += new_segment_weight;

                    DEBUG_GEOMETRY_EDGE( 
                            new_segment_weight, 
                            segment_length,
                            previous_osm_node_id,
                            this_osm_node_id);
                }
                else
                {
                    // If no lookup found, use the original weight value for this segment
                    new_weight += segment_weight;

                    DEBUG_GEOMETRY_EDGE(
                            segment_weight,
                            segment_length,
                            previous_osm_node_id,
                            this_osm_node_id);
                }

                previous_osm_node_id = this_osm_node_id;
            }

            inbuffer.weight = fixed_penalty + new_weight;
        }

        edge_based_edge_list.emplace_back(std::move(inbuffer));
    }

    DEBUG_GEOMETRY_STOP();
    SimpleLogger().Write() << "Done reading edges";
    return max_edge_id;
}

void Prepare::ReadNodeLevels(std::vector<float> &node_levels) const
{
    boost::filesystem::ifstream order_input_stream(config.level_output_path, std::ios::binary);

    unsigned level_size;
    order_input_stream.read((char *)&level_size, sizeof(unsigned));
    node_levels.resize(level_size);
    order_input_stream.read((char *)node_levels.data(), sizeof(float) * node_levels.size());
}

void Prepare::WriteNodeLevels(std::vector<float> &&in_node_levels) const
{
    std::vector<float> node_levels(std::move(in_node_levels));

    boost::filesystem::ofstream order_output_stream(config.level_output_path, std::ios::binary);

    unsigned level_size = node_levels.size();

    SimpleLogger().Write() << "Generating `myNodeLevels.txt`...";

    std::ofstream myNodeLevelsTxtFile;
    myNodeLevelsTxtFile.open("myNodeLevels.txt");

    myNodeLevelsTxtFile << "level" << std::endl;

    order_output_stream.write((char *)&level_size, sizeof(unsigned));
    order_output_stream.write((char *)node_levels.data(), sizeof(float) * node_levels.size());

    for (unsigned i = 0; i < level_size; i++)
        myNodeLevelsTxtFile << node_levels[i] << std::endl;

    myNodeLevelsTxtFile.close();
}

void Prepare::WriteCoreNodeMarker(std::vector<bool> &&in_is_core_node) const
{
    std::vector<bool> is_core_node(std::move(in_is_core_node));
    std::vector<char> unpacked_bool_flags(std::move(is_core_node.size()));
    for (auto i = 0u; i < is_core_node.size(); ++i)
    {
        unpacked_bool_flags[i] = is_core_node[i] ? 1 : 0;
    }

    boost::filesystem::ofstream core_marker_output_stream(config.core_output_path,
                                                          std::ios::binary);
    unsigned size = unpacked_bool_flags.size();
    core_marker_output_stream.write((char *)&size, sizeof(unsigned));
    core_marker_output_stream.write((char *)unpacked_bool_flags.data(),
                                    sizeof(char) * unpacked_bool_flags.size());
}

std::size_t Prepare::WriteContractedGraph(unsigned max_node_id,
                                          const DeallocatingVector<QueryEdge> &contracted_edge_list)
{
    // Sorting contracted edges in a way that the static query graph can read some in in-place.
    tbb::parallel_sort(contracted_edge_list.begin(), contracted_edge_list.end());
    const unsigned contracted_edge_count = contracted_edge_list.size();
    SimpleLogger().Write() << "Serializing compacted graph of " << contracted_edge_count
                           << " edges";

    const FingerPrint fingerprint = FingerPrint::GetValid();
    boost::filesystem::ofstream hsgr_output_stream(config.graph_output_path, std::ios::binary);
    hsgr_output_stream.write((char *)&fingerprint, sizeof(FingerPrint));
    const unsigned max_used_node_id = [&contracted_edge_list]
    {
        unsigned tmp_max = 0;
        for (const QueryEdge &edge : contracted_edge_list)
        {
            BOOST_ASSERT(SPECIAL_NODEID != edge.source);
            BOOST_ASSERT(SPECIAL_NODEID != edge.target);
            tmp_max = std::max(tmp_max, edge.source);
            tmp_max = std::max(tmp_max, edge.target);
        }
        return tmp_max;
    }();

    SimpleLogger().Write(logDEBUG) << "input graph has " << (max_node_id + 1) << " nodes";
    SimpleLogger().Write(logDEBUG) << "contracted graph has " << (max_used_node_id + 1) << " nodes";

    std::vector<StaticGraph<EdgeData>::NodeArrayEntry> node_array;
    // make sure we have at least one sentinel
    node_array.resize(max_node_id + 2);

    SimpleLogger().Write() << "Building node array";
    StaticGraph<EdgeData>::EdgeIterator edge = 0;
    StaticGraph<EdgeData>::EdgeIterator position = 0;
    StaticGraph<EdgeData>::EdgeIterator last_edge;

    // initializing 'first_edge'-field of nodes:
    for (const auto node : osrm::irange(0u, max_used_node_id + 1))
    {
        last_edge = edge;
        while ((edge < contracted_edge_count) && (contracted_edge_list[edge].source == node))
        {
            ++edge;
        }
        node_array[node].first_edge = position; //=edge
        position += edge - last_edge;           // remove
    }

    for (const auto sentinel_counter :
         osrm::irange<unsigned>(max_used_node_id + 1, node_array.size()))
    {
        // sentinel element, guarded against underflow
        node_array[sentinel_counter].first_edge = contracted_edge_count;
    }

    SimpleLogger().Write() << "Serializing node array";

    RangebasedCRC32 crc32_calculator;
    const unsigned edges_crc32 = crc32_calculator(contracted_edge_list);
    SimpleLogger().Write() << "Writing CRC32: " << edges_crc32;

    const unsigned node_array_size = node_array.size();
    // serialize crc32, aka checksum
    hsgr_output_stream.write((char *)&edges_crc32, sizeof(unsigned));
    // serialize number of nodes
    hsgr_output_stream.write((char *)&node_array_size, sizeof(unsigned));
    // serialize number of edges
    hsgr_output_stream.write((char *)&contracted_edge_count, sizeof(unsigned));
    // serialize all nodes
    if (node_array_size > 0)
    {
        hsgr_output_stream.write((char *)&node_array[0],
                                 sizeof(StaticGraph<EdgeData>::NodeArrayEntry) * node_array_size);
    }

    // serialize all edges
    SimpleLogger().Write() << "Building edge array";
    int number_of_used_edges = 0;

    SimpleLogger().Write() << "Generating `myContractedEdges.txt`...";

    std::ofstream myContractedEdgesTxtFile;
    myContractedEdgesTxtFile.open("myContractedEdges.txt");

    myContractedEdgesTxtFile << "source\ttarget\tdistance\tforward\tbackward" << std::endl;

    StaticGraph<EdgeData>::EdgeArrayEntry current_edge;
    for (const auto edge : osrm::irange<std::size_t>(0, contracted_edge_list.size()))
    {
        // no eigen loops
        BOOST_ASSERT(contracted_edge_list[edge].source != contracted_edge_list[edge].target);
        current_edge.target = contracted_edge_list[edge].target;
        current_edge.data = contracted_edge_list[edge].data;

        // every target needs to be valid
        BOOST_ASSERT(current_edge.target <= max_used_node_id);
#ifndef NDEBUG
        if (current_edge.data.distance <= 0)
        {
            SimpleLogger().Write(logWARNING) << "Edge: " << edge
                                             << ",source: " << contracted_edge_list[edge].source
                                             << ", target: " << contracted_edge_list[edge].target
                                             << ", dist: " << current_edge.data.distance;

            SimpleLogger().Write(logWARNING) << "Failed at adjacency list of node "
                                             << contracted_edge_list[edge].source << "/"
                                             << node_array.size() - 1;
            return 1;
        }
#endif
        hsgr_output_stream.write((char *)&current_edge,
                                 sizeof(StaticGraph<EdgeData>::EdgeArrayEntry));

        myContractedEdgesTxtFile << contracted_edge_list[edge].source << "\t" << contracted_edge_list[edge].target << "\t" << current_edge.data.distance << "\t" << current_edge.data.forward << "\t" << current_edge.data.backward << std::endl;

        ++number_of_used_edges;
    }

    myContractedEdgesTxtFile.close();

    return number_of_used_edges;
}

/**
 \brief Build contracted graph.
 */
void Prepare::ContractGraph(const unsigned max_edge_id,
                            DeallocatingVector<EdgeBasedEdge> &edge_based_edge_list,
                            DeallocatingVector<QueryEdge> &contracted_edge_list,
                            std::vector<bool> &is_core_node,
                            std::vector<float> &inout_node_levels) const
{
    std::vector<float> node_levels;
    node_levels.swap(inout_node_levels);

    Contractor contractor(max_edge_id + 1, edge_based_edge_list, std::move(node_levels));
    contractor.Run(config.core_factor);
    contractor.GetEdges(contracted_edge_list);
    contractor.GetCoreMarker(is_core_node);
    contractor.GetNodeLevels(inout_node_levels);
}
