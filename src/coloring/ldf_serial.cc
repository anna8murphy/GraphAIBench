#include "graph.h"

void ColorSolver(Graph &g, int *colors) {
  g.orientation();
  // reverse graph edges to start at degree 0 notes
  g.print_graph();
  Timer t;
  t.Start();
  int max_color = 0;
  std::vector<int> mark(g.V(), -1);
  std::cout << "LDF vertex coloring\n";
  for (vidType u = 0; u < g.V(); u++) {
    for (auto v : g.N(u))
      mark[colors[v]] = u;
    int vertex_color = 0;
    while (vertex_color < max_color && mark[vertex_color] == u)
      vertex_color++;
    if (vertex_color == max_color)
      max_color++;
    colors[u] = vertex_color;
  }
  t.Stop();
  std::cout << "runtime [serial] = " << t.Seconds() << " sec\n";
  std::cout << "Final Coloring:\n";
  for (vidType u = 0; u < g.V(); u++) {
    std::cout << "Vertex " << u << " is colored with " << colors[u] << std::endl;
  }
}
