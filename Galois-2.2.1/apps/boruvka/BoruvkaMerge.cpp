/** Boruvka application -*- C++ -*-
 * @file
 * @section License
 *
 * Galois, a framework to exploit amorphous data-parallelism in irregular
 * programs.
 *
 * Copyright (C) 2011, The University of Texas at Austin. All rights reserved.
 * UNIVERSITY EXPRESSLY DISCLAIMS ANY AND ALL WARRANTIES CONCERNING THIS
 * SOFTWARE AND DOCUMENTATION, INCLUDING ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR ANY PARTICULAR PURPOSE, NON-INFRINGEMENT AND WARRANTIES OF
 * PERFORMANCE, AND ANY WARRANTY THAT MIGHT OTHERWISE ARISE FROM COURSE OF
 * DEALING OR USAGE OF TRADE.  NO WARRANTY IS EITHER EXPRESS OR IMPLIED WITH
 * RESPECT TO THE USE OF THE SOFTWARE OR DOCUMENTATION. Under no circumstances
 * shall University be liable for incidental, special, indirect, direct or
 * consequential damages or loss of profits, interruption of business, or
 * related expenses which may arise from use of Software or Documentation,
 * including but not limited to those resulting from defects in Software and/or
 * Documentation, or loss or inaccuracy of data of any kind.
 *
 * @author Donald Nguyen <ddn@cs.utexas.edu>
 * @author Rashid Kaleem <rashid@cs.utexas.edu>
 */

#include "Galois/Statistic.h"
#include "Galois/Graph/Graph.h"
#include "Galois/Timer.h"
#include "Galois/Galois.h"
#include "Galois/Graph/LCGraph.h"

#include "llvm/Support/CommandLine.h"
#include "Lonestar/BoilerPlate.h"
#ifdef GALOIS_USE_EXP
#include "Galois/PriorityScheduling.h"
#endif

#include <string>
#include <sstream>
#include <limits>
#include <iostream>
#include <fstream>
#include <map>
#include <set>
#include <vector>

#define BORUVKA_DEBUG 0
#define COMPILE_STATISICS 0
#if BORUVKA_DEBUG
#include "UnionFind.h"
#endif
#if COMPILE_STATISICS
#include <time.h>
int BORUVKA_SAMPLE_FREQUENCY= 1000000;
#endif

#include "chunk_size.h"
using namespace std;
namespace cll = llvm::cl;

static const char* name = "Boruvka's Minimum Spanning Tree Algorithm";
static const char* desc = "Computes a minimum weight spanning tree of a graph";
static const char* url = "mst";

static cll::opt<std::string> inputfile(cll::Positional, cll::desc("<input file>"), cll::Required);
static cll::opt<bool> use_weighted_rmat("wrmat",cll::desc("Weighted RMAT"), cll::Optional,cll::init(false));
static cll::opt<bool> verify_via_kruskal("verify",cll::desc("Verify MST result via Serial Kruskal"), cll::Optional,cll::init(false));
static cll::opt<int> stepShift("delta", cll::desc("Shift value for the deltastep"), cll::init(0));
static cll::opt<std::string> worklistname("wl", cll::desc("Worklist to use"), cll::value_desc("worklist"), cll::init("obim"));
static cll::opt<std::string> resultFile("resultFile", cll::desc("File for writting experiment results"), cll::init("result.txt"));
static cll::opt<std::string> mqSuff("suff", cll::desc("Suffix for amq or smq"), cll::init(""));


static const bool trackWork = true;
static Galois::Statistic* nOverall;
static Galois::Statistic* BadWork;
static Galois::Statistic* WLEmptyWork;
static Galois::Statistic* nBad;
static Galois::Statistic* nEmpty;
static Galois::Statistic* nEdgesProcessed;
static Galois::Statistic* nNodesProcessed;

static int nodeID = 0;
///////////////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////////////
struct Node {
   //Do not include node data if not debugging since
   //it is a useless overhead. Useful for debugging though.
#if BORUVKA_DEBUG
   int id;
   Node(int i=-1) :
      id(i) {
   }
   std::string toString() {
      std::ostringstream s;
      s << "N(" << id << ")";
      return s.str();
   }
#else
   Node(int){};
   Node(){};
#endif
};
std::ostream& operator<<(std::ostream& s, Node& n) {
#if BORUVKA_DEBUG
   s << n.toString();
#endif
   return s;
}
///////////////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////////////
typedef long NodeDataType;
typedef int EdgeDataType;

typedef Galois::Graph::FirstGraph<Node, EdgeDataType, false> Graph;
typedef Graph::GraphNode GNode;
//The graph.
Graph graph;
///////////////////////////////////////////////////////////////////////////////////////

///////////////////////////////////////////////////////////////////////////////////////
#if COMPILE_STATISICS
struct GraphStats{
   std::vector<float> average_degrees;
   std::vector<int> time_vals;
   std::vector<unsigned long> max_degrees;
   unsigned long counter;
   GraphStats(){
      counter=0;
   }
   GraphStats &  tick(){
      ++counter;
      return *this;
   }
   void snap(){
      unsigned long num_nodes=0;
      unsigned long num_edges=0;
      unsigned long current_degree = 0;
      unsigned long max_degree=0;
      for(Graph::iterator it = graph.begin(), end_it = graph.end(); it!=end_it; ++it){
         ++num_nodes;
         current_degree=0;
         for(Graph::edge_iterator e_it = graph.edge_begin(*it, Galois::MethodFlag::NONE), e_it_end = graph.edge_end(*it, Galois::MethodFlag::NONE); e_it!=e_it_end; ++e_it){
            ++num_edges;
            ++current_degree;
         }
         if(current_degree > max_degree) max_degree = current_degree;
      }
      time_vals.push_back(time(0));
      average_degrees.push_back((float)(num_edges)/(num_nodes));
      max_degrees.push_back(max_degree);
   }
   void dump(ostream & out){
      out<<"\nMax degrees,";
      for(size_t i=0;i<max_degrees.size(); ++i){
         out << " " << max_degrees[i]<<",";
      }
      out<<"\nAverage degrees,";
      for(size_t i=0;i<average_degrees.size(); ++i){
         out << " " << average_degrees[i]<<",";
      }
      out<<"\nTime, ";
      for(size_t i=0;i<time_vals.size(); ++i){
         out << " " << time_vals[i]<<",";
      }
      out<<"\n";
   }
};
GraphStats stat_collector;
#endif

/////////////////////////////////////////////////////////////////////////////////////////
void printGraph() {
   int numEdges = 0;
      for (Graph::iterator src = graph.begin(), esrc = graph.end(); src != esrc; ++src) {
      Node& sdata = graph.getData(*src, Galois::MethodFlag::NONE);
      if (graph.containsNode(*src))
         for (Graph::edge_iterator dst = graph.edge_begin(*src, Galois::MethodFlag::NONE), edst = graph.edge_end(*src, Galois::MethodFlag::NONE); dst != edst; ++dst) {
            EdgeDataType w = graph.getEdgeData(dst);
            assert(w>=0);
            Node & ddata = graph.getData(graph.getEdgeDst(dst));
            std::cout << "1) " << sdata << " => " << ddata << " [ " << w << " ] " << std::endl;
            numEdges++;
         }
   }
   std::cout << "Num edges " << numEdges << std::endl;
}
///////////////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////////////

struct WorkItem{
  GNode first;
  unsigned second;

  unsigned prior() const {
    return second;
  }

  WorkItem(const GNode& N, unsigned W): first(N), second(W) {}

  WorkItem(): first(), second(0) {}

  unsigned int operator() () const {
    return second;
  }

  bool operator==(const WorkItem& other) const {
    return second == other.second && first == other.first;
  }
};

struct Indexer: public std::unary_function<WorkItem, unsigned> {
   unsigned operator()(const WorkItem& n) {
      unsigned t = stepShift ? (n.second >> stepShift) : n.second;
      return t;
   }
   static unsigned foo(const GNode& n) {
      unsigned d = std::distance(graph.edge_begin(n, Galois::MethodFlag::NONE), graph.edge_end(n, Galois::MethodFlag::NONE));
      unsigned t = stepShift ? (d >> stepShift) : d;
      return t;
   }
};


template<typename UpdateRequest, size_t N>
struct ParameterizedUpdateRequestIndexer: public std::unary_function<UpdateRequest, unsigned> {
   unsigned operator()(const UpdateRequest& n) {
      unsigned t = N ? (n.second >> N) : n.second;
      return t;
   }
};



Galois::Runtime::PerThreadStorage<long long> MSTWeight;
struct process {
   template<typename ContextTy>
   void operator()(const WorkItem& srcn, ContextTy& lwl) {
     *nNodesProcessed += 1;
      GNode src = srcn.first;
      if (graph.containsNode(src) == false) {
         // if src does not exist in graph, empty work
         *nEmpty += 1;
         *WLEmptyWork += lwl.t.stopwatch();
         return;
      }
      graph.getData(src, Galois::MethodFlag::ALL);
      GNode * minNeighbor = nullptr;
#if BORUVKA_DEBUG
      //std::cout<<"Processing "<<graph.getData(src).toString()<<std::endl;
#endif
      EdgeDataType minEdgeWeight = std::numeric_limits<EdgeDataType>::max();
      //Acquire locks on neighborhood.
      for (Graph::edge_iterator dst = graph.edge_begin(src, Galois::MethodFlag::ALL), edst = graph.edge_end(src, Galois::MethodFlag::ALL); dst != edst; ++dst) {
         graph.getData(graph.getEdgeDst(dst));
      }
      //Find minimum neighbor
      for (Graph::edge_iterator e_it = graph.edge_begin(src, Galois::MethodFlag::NONE), edst = graph.edge_end(src, Galois::MethodFlag::NONE); e_it != edst; ++e_it) {
         EdgeDataType w = graph.getEdgeData(e_it, Galois::MethodFlag::NONE);
         assert(w>=0);
         if (w < minEdgeWeight) {
            minNeighbor = &((*e_it).first());
            minEdgeWeight = w;
         }
      }
      //If there are no outgoing neighbors.
      if (minEdgeWeight == std::numeric_limits<EdgeDataType>::max()) {
         graph.removeNode(src, Galois::MethodFlag::NONE);

         *nOverall += 1;
         return;
      }
#if BORUVKA_DEBUG
            //std::cout << " Min edge from "<<graph.getData(src) << " to "<<graph.getData(*minNeighbor)<<" " <<minEdgeWeight << " "<<std::endl;
#endif
      int nEdge = 0;
      //Acquire locks on neighborhood of min neighbor.
      for (Graph::edge_iterator e_it = graph.edge_begin(*minNeighbor, Galois::MethodFlag::ALL), edst = graph.edge_end(*minNeighbor, Galois::MethodFlag::ALL); e_it != edst; ++e_it, nEdge++) {
         graph.getData(graph.getEdgeDst(e_it));
      }
      assert(minEdgeWeight>=0);
      //update MST weight.
      *MSTWeight.getLocal() += minEdgeWeight;
      typedef std::pair<GNode, EdgeDataType> EdgeData;
      typedef std::set<EdgeData, std::less<EdgeData>, Galois::PerIterAllocTy::rebind<EdgeData>::other> edsetTy;
      edsetTy toAdd(std::less<EdgeData>(), Galois::PerIterAllocTy::rebind<EdgeData>::other(lwl.getPerIterAlloc()));
      for (Graph::edge_iterator mdst = graph.edge_begin(*minNeighbor, Galois::MethodFlag::NONE), medst = graph.edge_end(*minNeighbor, Galois::MethodFlag::NONE); mdst != medst; ++mdst) {
         GNode dstNode = graph.getEdgeDst(mdst);
         int edgeWeight = graph.getEdgeData(mdst,Galois::MethodFlag::NONE);
         if (dstNode != src) { //Do not add the edge being contracted
            Graph::edge_iterator dup_edge = graph.findEdge(src, dstNode, Galois::MethodFlag::NONE);
            if (dup_edge != graph.edge_end(src, Galois::MethodFlag::NONE)) {
               EdgeDataType dup_wt = graph.getEdgeData(dup_edge,Galois::MethodFlag::NONE);
                  graph.getEdgeData(dup_edge,Galois::MethodFlag::NONE) = std::min<EdgeDataType>(edgeWeight, dup_wt);
                  assert(std::min<EdgeDataType>(edgeWeight, dup_wt)>=0);
            } else {
                  toAdd.insert(EdgeData(dstNode, edgeWeight));
                  assert(edgeWeight>=0);
            }
         }
      }
      graph.removeNode(*minNeighbor, Galois::MethodFlag::NONE);
      int nAdd = 0;
      for (edsetTy::iterator it = toAdd.begin(), endIt = toAdd.end(); it != endIt; it++, nAdd++) {
         graph.getEdgeData(graph.addEdge(src, it->first, Galois::MethodFlag::NONE)) = it->second;
      }
      lwl.push(WorkItem(src, nEdge + nAdd));
#if COMPILE_STATISICS
      if(stat_collector.tick().counter%BORUVKA_SAMPLE_FREQUENCY==0)
         stat_collector.snap();
#endif
      *nOverall += 1;
      *nEdgesProcessed += nEdge;
   }
};
///////////////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////////////
typedef Galois::InsertBag<WorkItem> Bag;

struct InitialProcess {
  Graph& graph;
  Bag& bag;
  InitialProcess( Graph& g, Bag& b): graph(g), bag(b) { }
  void operator()(GNode n) {
    bag.push(WorkItem(n, Indexer::foo(n)));
  }
};

struct seq_less: public std::binary_function<const GNode&, const GNode&, bool> {
   bool operator()(const GNode& lhs, const GNode& rhs) const {
      if (Indexer::foo(lhs) < Indexer::foo(rhs))
         return true;
      if (Indexer::foo(lhs) > Indexer::foo(rhs))
         return false;
      return lhs < rhs;
   }
};
struct seq_gt: public std::binary_function<const WorkItem&, const WorkItem&, bool> {
   bool operator()(const WorkItem& lhs, const WorkItem& rhs) const {
      if (lhs.second > rhs.second)
         return true;
      if (lhs.second < rhs.second)
         return false;
      return lhs.first > rhs.first;
   }
};

template<typename UpdateRequest>
struct UpdateRequestComparer: public std::binary_function<const UpdateRequest&, const UpdateRequest&, unsigned> {
  unsigned operator()(const UpdateRequest& x, const UpdateRequest& y) const {
    return x.second > y.second;
  }
};

//End body of for-each.
///////////////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////////////
long long runBodyParallel() {
   using namespace Galois::WorkList;
   typedef WorkItem UpdateRequest;
   typedef UpdateRequestComparer<UpdateRequest> Comparer;

   typedef dChunkedFIFO<CHUNK_SIZE> Chunk;
   typedef dVisChunkedFIFO<64> visChunk;
   typedef dChunkedPTFIFO<1> noChunk;
   typedef ChunkedFIFO<64> globChunk;
   typedef ChunkedFIFO<1> globNoChunk;
   typedef OrderedByIntegerMetric<Indexer, Chunk, 10> OBIM;
   typedef AdaptiveOrderedByIntegerMetric<Indexer, Chunk, 0, true, false, CHUNK_SIZE> ADAPOBIM;
   typedef OrderedByIntegerMetric<Indexer, dChunkedLIFO<64>, 10> OBIM_LIFO;
   typedef OrderedByIntegerMetric<Indexer, Chunk, 4> OBIM_BLK4;
   typedef OrderedByIntegerMetric<Indexer, Chunk, 10, false> OBIM_NOBSP;
   typedef OrderedByIntegerMetric<Indexer, noChunk, 10> OBIM_NOCHUNK;
   typedef OrderedByIntegerMetric<Indexer, globChunk, 10> OBIM_GLOB;
   typedef OrderedByIntegerMetric<Indexer, globNoChunk, 10> OBIM_GLOB_NOCHUNK;
   typedef OrderedByIntegerMetric<Indexer, noChunk, -1, false> OBIM_STRICT;
   typedef OrderedByIntegerMetric<Indexer, Chunk, 10, true, true> OBIM_UBSP;
   typedef OrderedByIntegerMetric<Indexer, visChunk, 10, true, true> OBIM_VISCHUNK;
   //typedef WorkItem WorkItem;
   typedef GlobPQ<WorkItem, LockFreeSkipList<seq_gt, WorkItem>> GPQ;
   typedef GlobPQ<WorkItem, SprayList<seq_gt, WorkItem>> SL;
   typedef GlobPQ<WorkItem, MultiQueue<Comparer, WorkItem, 1>> MQ1;
   typedef GlobPQ<WorkItem, MultiQueue<Comparer, WorkItem, 2>> MQ2;
   typedef GlobPQ<WorkItem, MultiQueue<Comparer, WorkItem, 3>> MQ3;
   typedef GlobPQ<WorkItem, MultiQueue<Comparer, WorkItem, 4>> MQ4;
   typedef GlobPQ<WorkItem, MultiQueue<Comparer, WorkItem, 5>> MQ5;
   typedef MyHMQ<UpdateRequest, Comparer, 1, true> HMQ1;
   typedef MyHMQ<UpdateRequest, Comparer, 2, true> HMQ2;
   typedef MyHMQ<UpdateRequest, Comparer, 3, true> HMQ3;
   typedef MyHMQ<UpdateRequest, Comparer, 4, true> HMQ4;
   typedef MyHMQ<UpdateRequest, Comparer, 5, true> HMQ5;
   typedef MyHMQ<UpdateRequest, Comparer, 6, true> HMQ6;
   typedef MyHMQ<UpdateRequest, Comparer, 7, true> HMQ7;
   typedef MyHMQ<UpdateRequest, Comparer, 8, true> HMQ8;
   typedef GlobPQ<WorkItem, DistQueue<seq_gt, WorkItem, false>> PTSL;
   typedef GlobPQ<WorkItem, DistQueue<seq_gt, WorkItem, true>> PPSL;
   typedef GlobPQ<WorkItem, LocalPQ<seq_gt, WorkItem>> LPQ;
   typedef GlobPQ<WorkItem, SwarmPQ<seq_gt, WorkItem>> SWARMPQ;
   typedef GlobPQ<WorkItem, HeapSwarmPQ<seq_gt, WorkItem>> HSWARMPQ;
   typedef SkipListOrderedByIntegerMetric<Indexer, Chunk, 10> SLOBIM;
   typedef SkipListOrderedByIntegerMetric<Indexer, noChunk, 10> SLOBIM_NOCHUNK;
   typedef SkipListOrderedByIntegerMetric<Indexer, visChunk, 10> SLOBIM_VISCHUNK;
   typedef VectorOrderedByIntegerMetric<Indexer, Chunk, 10> VECOBIM;
   typedef VectorOrderedByIntegerMetric<Indexer, noChunk, 10> VECOBIM_NOCHUNK;
   typedef VectorOrderedByIntegerMetric<Indexer, globNoChunk, 10> VECOBIM_GLOB_NOCHUNK;
//  typedef SkipListSMQ<WorkItem, seq_gt, Prob<1, 2>, true> SMQ_1_2;
//  typedef SkipListSMQ<WorkItem, seq_gt, Prob<1, 16>, true> SMQ_1_16;

   size_t approxNodeData = graph.size() * 128;
   Galois::preAlloc(numThreads + 3 * approxNodeData / Galois::Runtime::MM::pageSize);
   Galois::reportPageAlloc("MeminfoPre");

#if BORUVKA_DEBUG
   std::cout<<"Graph size "<<graph.size()<<std::endl;
#endif
#if COMPILE_STATISICS
   BORUVKA_SAMPLE_FREQUENCY = graph.size()/200;//200 samples should be sufficient.
#endif
   for (size_t i = 0; i < MSTWeight.size(); i++){
      *MSTWeight.getRemote(i) = 0;
   }

   std::string wl = worklistname;
  if (!mqSuff.empty()) {
    mqSuff = "_" + mqSuff;
  }
   if (wl.find("obim") == std::string::npos)
     stepShift = 0;
   std::cout << "INFO: Using delta-step of " << (1 << stepShift) << "\n";

   Bag initial;
   std::cout<<"Starting......"<<std::endl;
   Galois::do_all(graph.begin(), graph.end(), InitialProcess(graph, initial));
   std::cout<<"Done initialization"<<std::endl;

   Galois::StatTimer T;
   T.start();

#define element_t UpdateRequest
#define RUN_WL(WL) Galois::for_each_local(initial, process(), Galois::wl<WL>())
#include "Experiments.h"
#ifdef GALOIS_USE_EXP
   Exp::PriAuto<64, Indexer, OBIM, seq_less, seq_gt>::for_each_local(graph.begin(), graph.end(), process());
#else
   if (wl == "obim")
   Galois::for_each_local(initial, process(), Galois::wl<OBIM>());
   else if (wl == "pmod")
   Galois::for_each_local(initial, process(), Galois::wl<ADAPOBIM>());
   else if (wl == "slobim")
     Galois::for_each_local(initial, process(), Galois::wl<SLOBIM>());
   else if (wl == "slobim-nochunk")
     Galois::for_each_local(initial, process(), Galois::wl<SLOBIM_NOCHUNK>());
   else if (wl == "slobim-vischunk")
     Galois::for_each_local(initial, process(), Galois::wl<SLOBIM_VISCHUNK>());
   else if (wl == "vecobim")
     Galois::for_each_local(initial, process(), Galois::wl<VECOBIM>());
   else if (wl == "vecobim-nochunk")
     Galois::for_each_local(initial, process(), Galois::wl<VECOBIM_NOCHUNK>());
   else if (wl == "vecobim-glob-nochunk")
     Galois::for_each_local(initial, process(), Galois::wl<VECOBIM_GLOB_NOCHUNK>());
   else if (wl == "obim-strict")
     Galois::for_each_local(initial, process(), Galois::wl<OBIM_STRICT>());
   else if (wl == "obim-ubsp")
     Galois::for_each_local(initial, process(), Galois::wl<OBIM_UBSP>());
   else if (wl == "obim-lifo")
     Galois::for_each_local(initial, process(), Galois::wl<OBIM_LIFO>());
   else if (wl == "obim-blk4")
     Galois::for_each_local(initial, process(), Galois::wl<OBIM_BLK4>());
   else if (wl == "obim-nobsp")
     Galois::for_each_local(initial, process(), Galois::wl<OBIM_NOBSP>());
   else if (wl == "obim-nochunk")
     Galois::for_each_local(initial, process(), Galois::wl<OBIM_NOCHUNK>());
   else if (wl == "obim-vischunk")
     Galois::for_each_local(initial, process(), Galois::wl<OBIM_VISCHUNK>());
   else if (wl == "obim-glob")
     Galois::for_each_local(initial, process(), Galois::wl<OBIM_GLOB>());
   else if (wl == "obim-glob-nochunk")
     Galois::for_each_local(initial, process(), Galois::wl<OBIM_GLOB_NOCHUNK>());
   else if (wl == "skiplist")
     Galois::for_each_local(initial, process(), Galois::wl<GPQ>());
   else if (wl == "spraylist")
     Galois::for_each_local(initial, process(), Galois::wl<SL>());
   else if (wl == "mq2")
     Galois::for_each_local(initial, process(), Galois::wl<MQ2>());
   else if (wl == "mq3")
     Galois::for_each_local(initial, process(), Galois::wl<MQ3>());
   else if (wl == "mq1")
     Galois::for_each_local(initial, process(), Galois::wl<MQ1>());
   else if (wl == "mq4")
     Galois::for_each_local(initial, process(), Galois::wl<MQ4>());
   else if (wl == "mq5")
     Galois::for_each_local(initial, process(), Galois::wl<MQ5>());
   else if (wl == "hmq2")
     Galois::for_each_local(initial, process(), Galois::wl<HMQ2>());
   else if (wl == "hmq3")
     Galois::for_each_local(initial, process(), Galois::wl<HMQ3>());
   else if (wl == "hmq1")
     Galois::for_each_local(initial, process(), Galois::wl<HMQ1>());
   else if (wl == "hmq4")
     Galois::for_each_local(initial, process(), Galois::wl<HMQ4>());
   else if (wl == "hmq5")
     Galois::for_each_local(initial, process(), Galois::wl<HMQ5>());
   else if (wl == "hmq6")
     Galois::for_each_local(initial, process(), Galois::wl<HMQ6>());
   else if (wl == "hmq7")
     Galois::for_each_local(initial, process(), Galois::wl<HMQ7>());
   else if (wl == "hmq8")
     Galois::for_each_local(initial, process(), Galois::wl<HMQ8>());
   else if (wl == "thrskiplist")
     Galois::for_each_local(initial, process(), Galois::wl<PTSL>());
   else if (wl == "pkgskiplist")
     Galois::for_each_local(initial, process(), Galois::wl<PPSL>());
   else if (wl == "lpq")
     Galois::for_each_local(initial, process(), Galois::wl<LPQ>());
   else if (wl == "swarm")
     Galois::for_each_local(initial, process(), Galois::wl<SWARMPQ>());
   else if (wl == "heapswarm")
     Galois::for_each_local(initial, process(), Galois::wl<HSWARMPQ>());
//   else
//     std::cerr << "No work list!" << "\n";
  typedef MyPQ<WorkItem, seq_gt, true> USUAL_PQ;
  if (worklistname == "pq")
    Galois::for_each_local(initial, process(), Galois::wl<USUAL_PQ>());

  typedef StealingMultiQueue<element_t, Comparer, 8, 8, true> smq_default;
  if (wl == "smq_default") RUN_WL(smq_default);

#endif
   T.stop();

  std::ofstream out(resultFile + mqSuff, std::ios::app);
  out << T.get() << ",";
  out.close();

   Galois::reportPageAlloc("MeminfoPost");
   Galois::Runtime::reportNumaAlloc("NumaPost");

   long long res = 0;
   for (size_t i = 0; i < MSTWeight.size(); i++) {
#if BORUVKA_DEBUG
      std::cout<<"MST +=" << *MSTWeight.getRemote(i)<<std::endl;
#endif
      res += *MSTWeight.getRemote(i);
   }
//   std::cout << "MST Weight is " << res <<std::endl;
   return res;
}
/////////////////////////////////////////////////////////////////////////////////////////
static void makeGraph(const char* input) {
   std::vector<GNode> nodes;
   //Create local computation graph.
   typedef Galois::Graph::LC_CSR_Graph<Node, EdgeDataType> InGraph;
   typedef InGraph::GraphNode InGNode;
   InGraph in_graph;
   //Read graph from file.
   Galois::Graph::readGraph(in_graph, input);
   std::cout << "Read " << in_graph.size() << " nodes\n";
   //A node and a int is an element.
   typedef std::pair<InGNode, EdgeDataType> Element;
   //A vector of element is 'Elements'
   typedef std::vector<Element> Elements;
   //A vector of 'Elements' is a 'Map'
   typedef std::vector<Elements> Map;
   //'in_edges' is a vector of vector of pairs of nodes and int.
   Map edges(in_graph.size());
   //
   int numEdges = 0;
   for (InGraph::iterator src = in_graph.begin(), esrc = in_graph.end(); src != esrc; ++src) {
      for (InGraph::edge_iterator dst = in_graph.edge_begin(*src, Galois::MethodFlag::NONE), edst = in_graph.edge_end(*src, Galois::MethodFlag::NONE); dst != edst; ++dst) {
         if (*src == *dst) {
#if BORUVKA_DEBUG
            std::cout<<"ERR:: Self loop at "<<*src<<std::endl;
#endif
            continue;
         }
         EdgeDataType w = in_graph.getEdgeData(dst);
         Element e(*src, w);
         edges[in_graph.getEdgeDst(dst)].push_back(e);
         numEdges++;
      }
   }
#if BORUVKA_DEBUG
   std::cout<<"Number of edges "<<numEdges<<std::endl;
#endif
   nodes.resize(in_graph.size());
   for (Map::iterator i = edges.begin(), ei = edges.end(); i != ei; ++i) {
      Node n(nodeID);
      GNode node = graph.createNode(n);
      graph.addNode(node);
      nodes[nodeID] = node;
      nodeID++;
   }

   int id = 0;
   numEdges = 0;
   EdgeDataType edge_sum = 0;
   int numDups = 0;
   for (Map::iterator i = edges.begin(), ei = edges.end(); i != ei; ++i) {
      GNode src = nodes[id];
      for (Elements::iterator j = i->begin(), ej = i->end(); j != ej; ++j) {
         Graph::edge_iterator it = graph.findEdge(src, nodes[j->first], Galois::MethodFlag::NONE);
         if (it != graph.edge_end(src, Galois::MethodFlag::NONE)) {
            numDups++;
            EdgeDataType w = (graph.getEdgeData(it));
            if (j->second < w) {
               graph.getEdgeData(it) = j->second;
               edge_sum += (j->second-w);
            }
         } else {
            graph.getEdgeData(graph.addEdge(src, nodes[j->first], Galois::MethodFlag::NONE)) = j->second;
            edge_sum += j->second;
         }
         numEdges++;
         assert(edge_sum < std::numeric_limits<EdgeDataType>::max());
      }
      id++;
   }
#if BORUVKA_DEBUG
   std::cout << "Final num edges " << numEdges << " Dups " << numDups << " sum :" << edge_sum << std::endl;
#endif
}
//////////////////////////////////////////////////////////////////////////////////////////
//////////////////////////////////RMAT Reading function//////////////////////////////////
template<typename NdTy, typename EdTy>
struct EdgeTuple{
   NdTy src;
   NdTy dst;
   EdTy wt;
   EdgeTuple(NdTy s, NdTy d, EdTy w):src(s),dst(d),wt(w){};
   void print()const{
      cout << "[" << src << ", " << dst << " , " << wt << "]\n";
   }
};
template<typename NdTy, typename EdTy>
struct LessThanEdgeTuple{
bool operator()(const EdgeTuple<NdTy,EdTy> & o1, const EdgeTuple<NdTy,EdTy> &o2){
   return (o1.wt==o2.wt)? o1.src < o2.src : o1.wt<o2.wt;
}
};
template<typename NdTy, typename EdTy>
std::ostream & operator << (std::ostream & out , const EdgeTuple<NdTy,EdTy> & e ){
   e.print();
   return out;
}
static void readWeightedRMAT(const char* input) {
   std::vector<EdgeTuple<NodeDataType,EdgeDataType> > et;
   et.reserve(100000000);
   ifstream inFile (input);
   NodeDataType src, dst;
   EdgeDataType wt;
   char header[30];
   inFile.seekg(0, ios::beg);
   inFile>>header;
   NodeDataType max_id=0;
   while(inFile.eof()==false){
      inFile>>src;
      inFile>>dst;
      inFile>>wt;
      max_id = max(max_id, (src>dst?src:dst));
      et.push_back(EdgeTuple<NodeDataType, EdgeDataType>(src,dst,wt));
   }

   std::vector<GNode> nodes;

   nodes.resize(max_id+1);
   for (NodeDataType l = 0; l < max_id+1 ; ++l) {
      Node n(nodeID);
      GNode node = graph.createNode(n);
      nodes[nodeID] = node;
      nodeID++;
   }

   long numEdges = 0;
   EdgeDataType edge_sum = 0;
   int numDups = 0;
   for (std::vector<EdgeTuple<NodeDataType, EdgeDataType> >::iterator eIt = et.begin(), end = et.end(); eIt!=end; ++eIt) {
      EdgeTuple<NodeDataType,EdgeDataType> e = *eIt;
      Graph::edge_iterator it = graph.findEdge(nodes[e.src], nodes[e.dst], Galois::MethodFlag::NONE);
         if (it != graph.edge_end(nodes[e.src], Galois::MethodFlag::NONE)) {
            numDups++;
            EdgeDataType w = (graph.getEdgeData(it));
            if (e.wt < w) {
               graph.getEdgeData(it) = e.wt;
               edge_sum += (e.wt-w);
            }
         } else {
            graph.getEdgeData(graph.addEdge(nodes[e.src], nodes[e.dst], Galois::MethodFlag::NONE)) = e.wt;
            edge_sum += e.wt;
         }
         numEdges++;
         assert(edge_sum < std::numeric_limits<EdgeDataType>::max());
   }
}

////////////////////////////End READ WRMAT////////////////////////////////////////////////
////////////////////////////Kruskal////////////////////////////////////////////////
#if BORUVKA_DEBUG
typedef EdgeTuple<NodeDataType, EdgeDataType> KEdgeTuple;
typedef std::vector<KEdgeTuple> KruskalGraph;
KruskalGraph read_edges(Graph  &g){
   KruskalGraph  * ret = new KruskalGraph ();
for(Graph::iterator it = g.begin(), e = g.end(); it!=e; ++it){
   for(Graph::edge_iterator e_it = g.edge_begin(*it), e_end = g.edge_end(*it); e_it!=e_end; ++e_it){
      ret->push_back(KEdgeTuple(g.getData(*it).id,g.getData(g.getEdgeDst(e_it)).id, g.getEdgeData(e_it) ));
   }
}
std::cout<<"Number of edge tuples " << ret->size() << "\n";
return *ret;
}
long long kruskal_impl(const size_t num_nodes, KruskalGraph kg){
   std::sort(kg.begin(), kg.end(), LessThanEdgeTuple<NodeDataType,EdgeDataType>());
   UnionFind<NodeDataType,-1> uf(num_nodes);
   size_t mst_size = 0;
   long long mst_sum = 0;
   for(size_t i = 0; i < kg.size(); ++i){
      KEdgeTuple e  = kg[i];
      NodeDataType src = uf.uf_find(e.src);
      NodeDataType dst = uf.uf_find(e.dst);
      if(src!=dst){
         uf.uf_union(src,dst);
         mst_sum+=e.wt;
         mst_size++;
         if(mst_size>=num_nodes-1)
            return mst_sum;
      }
   }
   return mst_sum;
   return -1;
}
long long verify(Graph & g){
   return kruskal_impl(g.size(), read_edges(g));
}
#endif

uint64_t getStatVal(Galois::Statistic* value) {
  uint64_t stat = 0;
  for (unsigned x = 0; x < Galois::Runtime::activeThreads; ++x)
    stat += value->getValue(x);
  return stat;
}

//////////////////////////////////////////////////////////////////////////////////////////
int main(int argc, char **argv) {
   Galois::StatManager M;
   LonestarStart(argc, argv, name, desc, url);
   if(use_weighted_rmat)
      readWeightedRMAT(inputfile.c_str());
   else
      makeGraph(inputfile.c_str());

   if (trackWork) {
     nOverall = new Galois::Statistic("nOverall");
     WLEmptyWork = new Galois::Statistic("EmptyWork");
     nEmpty = new Galois::Statistic("nEmpty");
     nBad = new Galois::Statistic("nBad");
     BadWork = new Galois::Statistic("BadWork");
     nOverall = new Galois::Statistic("nOverall");
     nEdgesProcessed = new Galois::Statistic("nEdgesProcessed");
     nNodesProcessed = new Galois::Statistic("nNodesProcessed");
   }
#if BORUVKA_DEBUG
   long long kruskal_wt;
   if(verify_via_kruskal){
      kruskal_wt= verify(graph);
      cout<<"Kruskal MST Result is " << kruskal_wt <<"\n";
   }
#endif
   cout << "Starting loop body\n";
   long long mst_wt = runBodyParallel();
   cout<<"Boruvka MST Result is " << mst_wt <<"\n";
#if BORUVKA_DEBUG
   if(verify_via_kruskal){
      assert(kruskal_wt==mst_wt);
   }
#endif

   if (trackWork) {
     std::string wl = worklistname;
     if (wl.find("smq") == 0)
       wl = wl + mqSuff;
     std::ofstream nodes(resultFile + mqSuff, std::ios::app);
     nodes << wl << "," << getStatVal(nNodesProcessed) << ","
           << Galois::Runtime::activeThreads << "," << stepShift << std::endl;
     nodes.close();

     delete WLEmptyWork;
     delete nEmpty;
     delete nOverall;
     delete nEdgesProcessed;
     delete nNodesProcessed;
     delete nBad;
     delete BadWork;
   }

#if COMPILE_STATISICS
   cout<< " \n==================================================\n";
   stat_collector.dump(cout);
   cout<< " \n==================================================\n";
#endif
   return 0;
}
