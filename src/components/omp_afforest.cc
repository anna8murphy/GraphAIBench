// Copyright 2016, National University of Defense Technology
// Authors: Xuhao Chen <cxh@illinois.edu>
#include "graph.h"
#include "platform_atomics.h"
#include <random>

vidType SampleFrequentElement(int m, vidType *comp, int64_t num_samples = 1024);
void Link(vidType u, vidType v, vidType *comp);
void Compress(int m, vidType *comp);

void CCSolver(Graph &g, vidType *comp) {
  if (!g.has_reverse_graph()) {
    std::cout << "This algorithm requires the reverse graph constructed for directed graph\n";
    std::cout << "Please set reverse to 1 in the command line\n";
    exit(1);
  }
  auto m = g.V();
	int num_threads = 1;
	#pragma omp parallel
	{
	num_threads = omp_get_num_threads();
	}
  std::cout << "OpenMP Connected Components (" << num_threads << "threads\n";

	// Initialize each node to a single-node self-pointing tree
	#pragma omp parallel for
	for (int n = 0; n < m; n ++) comp[n] = n;

	Timer t;
	t.Start();
  int neighbor_rounds = 2;
	// Process a sparse sampled subgraph first for approximating components.
	// Sample by processing a fixed number of neighbors for each vertex
	for (int r = 0; r < neighbor_rounds; ++r) {
		#pragma omp parallel for
		for (vidType src = 0; src < m; src ++) {
			for (vidType dst : g.out_neigh(src, r)) {
				// Link at most one time if neighbor available at offset r
				Link(src, dst, comp);
				break;
			}
		}
		Compress(m, comp);
	}

	// Sample 'comp' to find the most frequent element -- due to prior
	// compression, this value represents the largest intermediate component
	vidType c = SampleFrequentElement(m, comp);

	// Final 'link' phase over remaining edges (excluding largest component)
	if (!g.is_directed()) {
		#pragma omp parallel for schedule(dynamic, 2048)
		for (vidType u = 0; u < m; u ++) {
			// Skip processing nodes in the largest component
			if (comp[u] == c) continue;
			// Skip over part of neighborhood (determined by neighbor_rounds)
			for (auto v : g.out_neigh(u, neighbor_rounds)) {
				Link(u, v, comp);
			}
		}
	} else {
		#pragma omp parallel for schedule(dynamic, 2048)
		for (vidType u = 0; u < m; u ++) {
			if (comp[u] == c) continue;
			for (auto v : g.out_neigh(u, neighbor_rounds)) {
				Link(u, v, comp);
			}
			// To support directed graphs, process reverse graph completely
			for (auto v : g.in_neigh(u)) {
				Link(u, v, comp);
			}
		}
	}
	// Finally, 'compress' for final convergence
	Compress(m, comp);
	t.Stop();

	//printf("\titerations = %d.\n", iter);
	printf("\truntime [omp_afforest] = %f ms.\n", t.Millisecs());
	return;
}

// Place nodes u and v in same component of lower component ID
void Link(vidType u, vidType v, vidType *comp) {
	vidType p1 = comp[u];
	vidType p2 = comp[v];
	while (p1 != p2) {
		vidType high = p1 > p2 ? p1 : p2;
		vidType low = p1 + (p2 - high);
		vidType p_high = comp[high];
		// Was already 'low' or succeeded in writing 'low'
		if ((p_high == low) || (p_high == high && compare_and_swap(comp[high], high, low)))
			break;
		p1 = comp[comp[high]];
		p2 = comp[low];
	}
}

// Reduce depth of tree for each component to 1 by crawling up parents
void Compress(int m, vidType *comp) {
	#pragma omp parallel for schedule(static, 2048)
	for (vidType n = 0; n < m; n++) {
		while (comp[n] != comp[comp[n]]) {
			comp[n] = comp[comp[n]];
		}
	}
}

