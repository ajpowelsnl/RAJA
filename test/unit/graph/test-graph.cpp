//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~//
// Copyright (c) 2016-21, Lawrence Livermore National Security, LLC
// and RAJA project contributors. See the RAJA/COPYRIGHT file for details.
//
// SPDX-License-Identifier: (BSD-3-Clause)
//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~//

///
/// Source file containing tests for Graph Constructors
///

#include "RAJA_test-base.hpp"

#include "RAJA/RAJA.hpp"

#include "RAJA_gtest.hpp"

#include "RAJA_test-graph-execpol.hpp"
#include "RAJA_test-forall-execpol.hpp"

#include <unordered_map>
#include <type_traits>
#include <random>
#include <numeric>
#include <algorithm>


// Basic Constructors

TEST( GraphBasicConstructorUnitTest, BasicConstructors )
{
  using GraphPolicy = RAJA::loop_graph;
  using GraphResource = RAJA::resources::Host;
  using graph_type = RAJA::expt::graph::DAG<GraphPolicy, GraphResource>;

  // default constructor
  graph_type g;

  ASSERT_TRUE( g.empty() );
}


// Basic Execution

TEST( GraphBasicExecUnitTest, EmptyExec )
{
  using GraphPolicy = RAJA::loop_graph;
  using GraphResource = RAJA::resources::Host;
  using graph_type = RAJA::expt::graph::DAG<GraphPolicy, GraphResource>;

  auto r = GraphResource::get_default();

  // default constructor
  graph_type g;

  // empty exec
  g.exec(r);
  r.wait();

  ASSERT_TRUE( g.empty() );
}


TEST( GraphBasicExecUnitTest, OneNodeExec )
{
  using GraphPolicy = RAJA::loop_graph;
  using GraphResource = RAJA::resources::Host;
  using graph_type = RAJA::expt::graph::DAG<GraphPolicy, GraphResource>;

  auto r = GraphResource::get_default();

  // default constructor
  graph_type g;

  g >> RAJA::expt::graph::Empty();

  ASSERT_FALSE( g.empty() );

  // 1-node exec
  RAJA::resources::Event e = g.exec(r);
  e.wait();

  ASSERT_FALSE( g.empty() );
}


TEST( GraphBasicExecUnitTest, FourNodeExec )
{
  using GraphPolicy = RAJA::loop_graph;
  using GraphResource = RAJA::resources::Host;
  using graph_type = RAJA::expt::graph::DAG<GraphPolicy, GraphResource>;

  auto r = GraphResource::get_default();

  // default constructor
  graph_type g;

  int count = 0;
  int order[4]{-1, -1, -1, -1};

  /*
   *    0
   *   / \
   *  1   2
   *   \ /
   *    3
   */

  auto& n0 = g  >> RAJA::expt::graph::Function([&](){ order[0] = count++; });
  auto& n1 = n0 >> RAJA::expt::graph::Function([&](){ order[1] = count++; });
  auto& n2 = n0 >> RAJA::expt::graph::Function([&](){ order[2] = count++; });
  auto& n3 = n1 >> RAJA::expt::graph::Function([&](){ order[3] = count++; });
  n2 >> n3;

  ASSERT_FALSE( g.empty() );

  ASSERT_EQ(count, 0);
  ASSERT_EQ(order[0], -1);
  ASSERT_EQ(order[1], -1);
  ASSERT_EQ(order[2], -1);
  ASSERT_EQ(order[3], -1);

  // 4-node diamond DAG exec
  g.exec(r);
  r.wait();

  ASSERT_FALSE( g.empty() );

  ASSERT_EQ(count, 4);
  ASSERT_LT(order[0], order[1]);
  ASSERT_LT(order[0], order[2]);
  ASSERT_LT(order[1], order[3]);
  ASSERT_LT(order[2], order[3]);
}


TEST( GraphBasicExecUnitTest, TwentyNodeExec )
{
  using GraphPolicy = RAJA::loop_graph;
  using GraphResource = RAJA::resources::Host;
  using graph_type = RAJA::expt::graph::DAG<GraphPolicy, GraphResource>;

  auto r = GraphResource::get_default();

  // default constructor
  graph_type g;

  int count = 0;
  int order[20]{-1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
                -1, -1, -1, -1, -1, -1, -1, -1, -1, -1};

  /*
   *  0__   1     2 3
   *  |  \ / \    |/ \
   *  4   5_ _6   7_ _8
   *  |__/|_X_|   |_X_|
   *  9__ 0   1   2   3
   *  |  \|   |\ /|   |
   *  4   5   6 7 8   9
   */

  auto& n0  = g   >> RAJA::expt::graph::Function([&](){ order[0]  = count++; });
  auto& n1  = g   >> RAJA::expt::graph::Function([&](){ order[1]  = count++; });
  auto& n2  = g   >> RAJA::expt::graph::Function([&](){ order[2]  = count++; });
  auto& n3  = g   >> RAJA::expt::graph::Function([&](){ order[3]  = count++; });

  auto& n4  = n0  >> RAJA::expt::graph::Function([&](){ order[4]  = count++; });
  auto& n5  = n0  >> RAJA::expt::graph::Function([&](){ order[5]  = count++; });
              n1  >> n5;
  auto& n6  = n1  >> RAJA::expt::graph::Function([&](){ order[6]  = count++; });
  auto& n7  = n2  >> RAJA::expt::graph::Function([&](){ order[7]  = count++; });
              n3  >> n7;
  auto& n8  = n3  >> RAJA::expt::graph::Function([&](){ order[8]  = count++; });

  auto& n9  = n4  >> RAJA::expt::graph::Function([&](){ order[9]  = count++; });
              n5  >> n9;
  auto& n10 = n5  >> RAJA::expt::graph::Function([&](){ order[10] = count++; });
              n6  >> n10;
  auto& n11 = n5  >> RAJA::expt::graph::Function([&](){ order[11] = count++; });
              n6  >> n11;
  auto& n12 = n7  >> RAJA::expt::graph::Function([&](){ order[12] = count++; });
              n8  >> n12;
  auto& n13 = n7  >> RAJA::expt::graph::Function([&](){ order[13] = count++; });
              n8  >> n13;

              n9  >> RAJA::expt::graph::Function([&](){ order[14]  = count++; });
  auto& n15 = n9  >> RAJA::expt::graph::Function([&](){ order[15]  = count++; });
              n10 >> n15;
              n11 >> RAJA::expt::graph::Function([&](){ order[16]  = count++; });
  auto& n17 = n11 >> RAJA::expt::graph::Function([&](){ order[17]  = count++; });
              n12 >> n17;
              n12 >> RAJA::expt::graph::Function([&](){ order[18]  = count++; });
              n13 >> RAJA::expt::graph::Function([&](){ order[19]  = count++; });

  ASSERT_FALSE( g.empty() );

  ASSERT_EQ(count, 0);
  ASSERT_EQ(order[0],  -1); ASSERT_EQ(order[1],  -1); ASSERT_EQ(order[2],  -1);
  ASSERT_EQ(order[3],  -1); ASSERT_EQ(order[4],  -1); ASSERT_EQ(order[5],  -1);
  ASSERT_EQ(order[6],  -1); ASSERT_EQ(order[7],  -1); ASSERT_EQ(order[8],  -1);
  ASSERT_EQ(order[9],  -1); ASSERT_EQ(order[10], -1); ASSERT_EQ(order[11], -1);
  ASSERT_EQ(order[12], -1); ASSERT_EQ(order[13], -1); ASSERT_EQ(order[14], -1);
  ASSERT_EQ(order[15], -1); ASSERT_EQ(order[16], -1); ASSERT_EQ(order[17], -1);
  ASSERT_EQ(order[18], -1); ASSERT_EQ(order[19], -1);

  // 8-node DAG exec
  g.exec(r);
  r.wait();

  ASSERT_FALSE( g.empty() );

  ASSERT_EQ(count, 20);
  ASSERT_LT(order[0],  order[4]);  ASSERT_LT(order[0], order[5]);
  ASSERT_LT(order[1],  order[5]);  ASSERT_LT(order[1], order[6]);
  ASSERT_LT(order[2],  order[7]);
  ASSERT_LT(order[3],  order[7]);  ASSERT_LT(order[3], order[8]);
  ASSERT_LT(order[4],  order[9]);
  ASSERT_LT(order[5],  order[9]);  ASSERT_LT(order[5], order[10]);  ASSERT_LT(order[5], order[11]);
  ASSERT_LT(order[6],  order[10]); ASSERT_LT(order[6], order[11]);
  ASSERT_LT(order[7],  order[12]); ASSERT_LT(order[7], order[13]);
  ASSERT_LT(order[8],  order[12]); ASSERT_LT(order[8], order[13]);
  ASSERT_LT(order[9],  order[14]); ASSERT_LT(order[9], order[15]);
  ASSERT_LT(order[10], order[15]);
  ASSERT_LT(order[11], order[16]); ASSERT_LT(order[11], order[17]);
  ASSERT_LT(order[12], order[17]); ASSERT_LT(order[12], order[18]);
  ASSERT_LT(order[13], order[19]);
}


template < typename graph_type >
struct RandomGraph
{
  using base_node_type = typename graph_type::base_node_type;

  static const int graph_min_nodes = 0;
  static const int graph_max_nodes = 1024;

  RandomGraph(unsigned seed)
    : m_rng(seed)
    , m_num_nodes(std::uniform_int_distribution<int>(graph_min_nodes, graph_max_nodes)(m_rng))
  {

  }

  std::vector<int> get_dependencies(int node_id)
  {
    assert(node_id < m_num_nodes);

    int num_edges_to_node = std::uniform_int_distribution<int>(0, node_id)(m_rng);

    // create a list of numbers from [0, node_id)
    std::vector<int> edges_to_node(node_id);
    std::iota(edges_to_node.begin(), edges_to_node.end(), 0);
    // randomly reorder the list
    std::shuffle(edges_to_node.begin(), edges_to_node.end(), m_rng);
    // remove extras
    edges_to_node.resize(num_edges_to_node);

    return edges_to_node;
  }

  // add a node
  // as a new disconnected component of the DAG
  // or with edges from some previous nodes
  // NOTE that this algorithm creates DAGs with more edges than necessary for
  // the required ordering
  //   Ex. a >> b, b >> c, a >> c where a >> c is unnecessary
  template < typename NodeArg >
  void add_node(int node_id, std::vector<int>&& edges_to_node, NodeArg&& arg)
  {
    assert(node_id < m_num_nodes);

    int num_edges_to_node = edges_to_node.size();

    base_node_type* n = nullptr;

    if (num_edges_to_node == 0) {

      // connect node to graph
      n = &(m_g >> std::forward<NodeArg>(arg));

    } else {

      // create edges
      // first creating node from an existing node
      n = &(*m_nodes[edges_to_node[0]] >> std::forward<NodeArg>(arg));
      m_edges.emplace(edges_to_node[0], node_id);

      // then adding other edges
      for (int i = 1; i < num_edges_to_node; ++i) {
        *m_nodes[edges_to_node[i]] >> *n;
        m_edges.emplace(edges_to_node[i], node_id);
      }
    }

    m_nodes.emplace_back(n);
  }

  int num_nodes() const
  {
    return m_num_nodes;
  }

  std::unordered_multimap<int, int> const& edges() const
  {
    return m_edges;
  }

  graph_type& graph()
  {
    return m_g;
  }

  ~RandomGraph() = default;

private:
  std::mt19937 m_rng;

  int m_num_nodes;

  std::unordered_multimap<int, int> m_edges;
  std::vector<base_node_type*> m_nodes;

  graph_type m_g;
};


inline unsigned get_random_seed()
{
  static unsigned seed = std::random_device{}();
  return seed;
}

TEST( GraphBasicExecUnitTest, RandomExec )
{
  using GraphPolicy = RAJA::loop_graph;
  using GraphResource = RAJA::resources::Host;
  using graph_type = RAJA::expt::graph::DAG<GraphPolicy, GraphResource>;

  auto r = GraphResource::get_default();

  unsigned seed = get_random_seed();

  RandomGraph<graph_type> g(seed);

  const int num_nodes = g.num_nodes();

  int count = 0;
  std::vector<int> order(num_nodes, -1);

  // add nodes
  for (int node_id = 0; node_id < num_nodes; ++node_id) {

    auto edges_to_node = g.get_dependencies(node_id);

    g.add_node(node_id, std::move(edges_to_node),
        RAJA::expt::graph::Function([&, node_id](){
      ASSERT_LE(0, node_id);
      ASSERT_LT(node_id, num_nodes);
      order[node_id] = count++;
    }));
  }

  ASSERT_FALSE( g.graph().empty() );

  // check graph has not executed
  ASSERT_EQ(count, 0);
  for (int i = 0; i < num_nodes; ++i) {
    ASSERT_EQ(order[i],  -1);
  }

  // check graph edges are valid
  for (std::pair<int, int> const& edge : g.edges()) {
    ASSERT_LE(0, edge.first);
    ASSERT_LT(edge.first, num_nodes);
    ASSERT_LE(0, edge.second);
    ASSERT_LT(edge.second, num_nodes);
    ASSERT_LT(edge.first, edge.second);
  }

  // 8-node DAG exec
  g.graph().exec(r);
  r.wait();

  // check graph has executed
  ASSERT_FALSE( g.graph().empty() );
  ASSERT_EQ(count, num_nodes);

  // check graph edges are valid
  for (std::pair<int, int> const& edge : g.edges()) {
    ASSERT_LE(0, order[edge.first]);
    ASSERT_LT(order[edge.first], num_nodes);
    ASSERT_LE(0, order[edge.second]);
    ASSERT_LT(order[edge.second], num_nodes);
    ASSERT_LT(order[edge.first], order[edge.second]);
  }
}