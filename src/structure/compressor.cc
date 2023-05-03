#include "compressor.hh"
#include "codecfactory.h"
#include "scan.h"
#include "cgr_encoder.hh"
#include "hybrid_encoder.hh"

#define CHECKPOINT 50000000

using namespace SIMDCompressionLib;

void Compressor::write_compressed_graph() {
  if (use_unary) {
    write_compressed_edges_to_disk();
  }
  std::cout << "Computing the row pointers\n";
  compute_ptrs();
  write_ptrs_to_disk();
}

void Compressor::compute_ptrs() {
  Timer t;
  t.Start();
  rowptr.resize(g->V()+1);
#if 0
  buffer.resize(g->V());
  #pragma omp parallel for
  for (vidType i = 0; i < g->V(); i++)
    buffer[i] = encoder->get_compressed_size(i).size();
  parallel_prefix_sum<vidType,eidType>(degrees, rowptr.data());
#else
  if (use_unary) {
    rowptr[0] = 0;
    for (vidType i = 0; i < g->V(); i++) {
      auto length = encoder->get_compressed_size(i);
      if (byte_aligned && length > 0)
        length = ((length-1)/8 + 1);
      if (word_aligned && length > 0)
        length = ((length-1)/32 + 1);
      rowptr[i+1] = length + rowptr[i];
    }
  } else {
    /*
    if (scheme == "hybrid" && byte_aligned) {
      std::cout << "Use byte pointers for the hybrid scheme\n";
      #pragma omp parallel for
      for (vidType i = 0; i < g->V(); i++)
        osizes[i] *= 4;
    }
    */
    parallel_prefix_sum<vidType,eidType>(osizes, rowptr.data());
  }
#endif
  t.Stop();
  std::cout << "Computing row pointers time: " << t.Seconds() << "\n";
}

void Compressor::write_ptrs_to_disk() {
  std::string filename = out_prefix + ".vertex.bin";
  std::cout << "Writing the row pointers to disk file " << filename << "\n";
  Timer t;
  t.Start();
  std::ofstream outfile((filename).c_str(), std::ios::binary);
  if (!outfile) {
    std::cout << "File not available\n";
    throw 1;
  }
  outfile.write(reinterpret_cast<const char*>(rowptr.data()), (g->V()+1)*sizeof(eidType));
  outfile.close();
  t.Stop();
  std::cout << "Writing row pointers time: " << t.Seconds() << "\n";
}

void Compressor::write_compressed_edges_to_disk() {
  std::string edge_file_name = out_prefix + ".edge.bin";
  std::cout << "writing the compressed edges to disk file " << edge_file_name << "\n";
  FILE *of_graph = fopen((edge_file_name).c_str(), "w");
  if (of_graph == 0) {
    std::cout << "graph file " << edge_file_name << " cannot be created!\n";
    abort();
  }

  Timer t;
  t.Start();
  std::vector<unsigned char> buf;
  unsigned char cur = 0;
  int bit_count = 0;
  int byte_count = 0, res_byte_count = 0;
  for (vidType i = 0; i < g->V(); i++) {
    auto deg = g->get_degree(i);
    if (scheme == "hybrid" && deg > degree_threshold) { // use VByte encoding
      auto cdata = encoder->get_compressed_words(i); // get compressed data handle
      if (fwrite(cdata.data(), sizeof(uint32_t), cdata.size(), of_graph) != cdata.size()) {
        perror("Could not write to file");
        exit(1);
        //printf("Number of items written to the file: %ld\n", num);
      }
    } else {
      auto &bit_array = encoder->get_compressed_bits(i);
      if (bit_array.size() == 0) continue;
      byte_count = (bit_array.size() - 1) / 8 + 1;
      res_byte_count = byte_count % 4;
      for (auto bit : bit_array) {
        cur <<= 1;
        if (bit) cur++;
        bit_count++;
        if (bit_count == 8) {
          buf.emplace_back(cur);
          cur = 0;
          bit_count = 0;
        }
      }
      if (bit_count && (word_aligned || byte_aligned)) {
        // byte alignment for each adj list
        while (bit_count < 8) cur <<= 1, bit_count++;
        buf.emplace_back(cur);
        bit_count = 0;
      }
      if (word_aligned && res_byte_count) {
        while (res_byte_count != 4) {
          cur = 0; // padding
          buf.emplace_back(cur);
          res_byte_count ++;
        }
      }
    }
  }
  // the last byte
  if (bit_count) {
    while (bit_count < 8) cur <<= 1, bit_count++;
    buf.emplace_back(cur);
  }
  if (word_aligned && use_permutate)
    permutate_bytes_by_word(buf);
  fwrite(buf.data(), sizeof(unsigned char), buf.size(), of_graph);
  t.Stop();
  std::cout << "Time writing compressed edges to disk: " << t.Seconds() << "\n";
  fclose(of_graph);
}

void Compressor::permutate_bytes_by_word(std::vector<unsigned char> &buf) {
  std::cout << "Permutating bytes in every word\n";
  auto num_bytes = buf.size();
  auto res_bytes = num_bytes % 4;
  assert(res_bytes == 0); // only does this when word aligned
  auto num_words = (num_bytes-res_bytes)/4 + (res_bytes?1:0);
  #pragma omp parallel for
  for (size_t i = 0; i < num_words; i++) {
    auto j = i*4;
    auto temp = buf[j];
    buf[j] = buf[j+3];
    buf[j+3] = temp;
    temp = buf[j+1];
    buf[j+1] = buf[j+2];
    buf[j+2] = temp;
  }
}
void Compressor::write_degrees() {
  std::cout << "Writing degrees to disk\n";
  Timer t;
  t.Start();
  std::ofstream outfile((out_prefix + ".degree.bin").c_str(), std::ios::binary);
  if (!outfile) {
    std::cout << "File not available\n";
    throw 1;
  }
  std::vector<vidType> degrees(g->V());
  for (vidType v = 0; v < g->V(); v++) degrees[v] = g->get_degree(v);
  outfile.write(reinterpret_cast<const char*>(degrees.data()), (g->V())*sizeof(vidType));
  outfile.close();
  t.Stop();
  std::cout << "Writing degrees time: " << t.Seconds() << "\n";
}

void Compressor::compress(bool pre_encode) {
  if (byte_aligned) std::cout << "Byte alignment enabled for each adj list\n";
  if (word_aligned) std::cout << "Word alignment enabled for each adj list\n";
  Timer t;
  if (use_unary && pre_encode) {
    std::cout << "Pre-encoding ...\n";
    t.Start();
    encoder->pre_encoding();
    t.Stop();
    std::cout << "Pre-encoding time: " << t.Seconds() << "\n";
  }
  if (scheme != "cgr") {
    osizes.resize(g->V());
  }
  vbyte_count = 0, unary_count = 0, trivial_count = 0;
  vbyte_adj_count = 0, unary_adj_count = 0;
  unary_bytes = 0, vbyte_bytes = 0;

  std::cout << "Start encoding\n"; 
  t.Start();
  if (use_unary) {
    //#pragma omp parallel for
    #pragma omp parallel for reduction(+:vbyte_count,unary_count,trivial_count,unary_bytes,vbyte_bytes) schedule(dynamic, 1)
    for (vidType v = 0; v < g->V(); v++) {
      if (v > 0 && v%CHECKPOINT==0)
        std::cout << "(" << v/CHECKPOINT << " * " << CHECKPOINT << ") vertices compressed\n";
      auto deg = g->get_degree(v);
      if (deg == 0) {
        trivial_count ++;
      }
      auto nbytes = encoder->encode(v, g->get_degree(v), g->N(v).data());
      if (scheme == "hybrid" && deg > degree_threshold) { // use VByte encoding
        vbyte_count ++;
        vbyte_adj_count += deg;
        vbyte_bytes += nbytes;
        osizes[v] = nbytes/4;
      } else { // use unary encoding
        unary_count ++;
        unary_adj_count += deg;
        unary_bytes += nbytes;
      }
    }
    encoder->print_stats();
  } else {
    FILE *of_graph = fopen((out_prefix + ".edge.bin").c_str(), "w");
    if (of_graph == 0) {
      std::cout << "graph file cannot create!" << std::endl;
      exit(1);
    }
    for (vidType v = 0; v < g->V(); v++) {
      auto deg = g->get_degree(v);
      if (buffer.size() < deg + 1024) {
        buffer.resize(deg + 1024);
      }
      size_t outsize = buffer.size();
      shared_ptr<IntegerCODEC> schemeptr = CODECFactory::getFromName(scheme);
      if (schemeptr.get() == NULL) exit(1);
      schemeptr->encodeArray(g->adj_ptr(v), deg, buffer.data(), outsize);
      osizes[v] = static_cast<vidType>(outsize);
      if (fwrite(buffer.data(), sizeof(vidType) * outsize, 1, of_graph) != 1) {
        std::cerr << "aborting" << std::endl;
        fclose(of_graph);
        exit(1);
      }
    }
    fclose(of_graph);
  }
  t.Stop();
  std::cout << "Encoding time: " << t.Seconds() << "\n";
}

void Compressor::print_stats() {
  std::cout << "vbyte_count: " << vbyte_count << " unary_count: " << unary_count << " trivial_count: " << trivial_count << "\n";
  float vbyte_rate = float(vbyte_adj_count)*4.0/float(vbyte_bytes);
  float unary_rate = float(unary_adj_count)*4.0/float(unary_bytes);
  std::cout << "VByte bytes: " << float(vbyte_bytes)/1024/1024 
            << " MB, original data: " << float(vbyte_adj_count)*4.0/1024/1024 
            << " MB, compression rate: " << vbyte_rate << "\n";
  std::cout << "Unary bytes: " << float(unary_bytes)/1024/1024 
            << " MB, original data: " << float(unary_adj_count)*4.0/1024/1024 
            << " MB, compression rate: " << unary_rate << "\n";
}

void printusage() {
  cout << "./compressor -s name-of-scheme <input_path> <output_path> [-z zeta_k(3)] \
                                                                     [-i use_interval] \
                                                                     [-p permutate_bytes] \
                                                                     [-d degree_threshold(32)] \
                                                                     [-a alignment(0)]\n";
}

int main(int argc,char *argv[]) {
  int zeta_k = 3, use_interval = 0, permutate = 0, degree_threshold = 32;
  int alignment = 0; // 0: not aligned; 1: byte aligned; 2: word aligned
  std::string scheme = "cgr";
  int c;
  while ((c = getopt(argc, argv, "s:z:ipa:d:h")) != -1) {
    switch (c) {
      case 's':
        scheme = optarg;
        //std::cout << "scheme: " << scheme << "\n";
        break;
      case 'z':
        zeta_k = atoi(optarg);
        //std::cout << "zeta_k: " << zeta_k << "\n";
        break;
      case 'i':
        use_interval = 1;
        //std::cout << "use_interval: " << use_interval << "\n";
        break;
      case 'p':
        permutate = 1;
        break;
      case 'a':
        alignment = atoi(optarg);
        //std::cout << "alignment: " << alignment << "\n";
        break;
      case 'd':
        degree_threshold = atoi(optarg);
        //std::cout << "degree_threshold: " << degree_threshold << "\n";
        break;
      case 'h':
        printusage();
        return 0;
      case '?': 
        printf("unknown option: %c\n", optopt);
        break;
      default:
        abort();
    }
  }
  if (optind + 1 >= argc) {
    printusage();
    return -1;
  }
 
  GraphTy g(argv[optind]);
  g.print_meta_data();

  bool use_unary = (scheme == "cgr" || scheme == "hybrid");
  std::cout << "Using the " << scheme << " compression scheme\n";
  if (scheme == "hybrid") {
    if (alignment != 2) std::cout << "hybrid scheme must be word-aligned\n";
    std::cout << "degree_threshold = " << degree_threshold << "\n";
  }

  unary_encoder *encoder = NULL;
  if (scheme == "cgr") {
    std::cout << "Creating a CGR encoder\n";
    encoder = new cgr_encoder(g.V(), zeta_k, use_interval);
  } else if (scheme == "hybrid") {
    std::cout << "Creating a hybrid encoder\n";
    encoder = new hybrid_encoder(g.V(), zeta_k, use_interval, degree_threshold);
  }
  if (alignment == 1) std::cout << "Config: byte alignment enabled for each adj list\n";
  Compressor compressor(scheme, argv[optind+1], use_unary, &g, encoder, permutate, degree_threshold, alignment);
  std::cout << "start compression ...\n";
  compressor.compress();
  std::cout << "writing compressed graph to disk ...\n";
  compressor.write_compressed_graph();
  if (scheme == "hybrid") {
    std::cout << "Start writing degrees to disk ...\n";
    compressor.write_degrees();
  } 
  std::cout << "compression completed!\n";
  return 0;
}
