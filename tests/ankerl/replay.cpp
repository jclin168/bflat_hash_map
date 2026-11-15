#include <algorithm>
#include <bflat/unordered_flat_map.hpp>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <vector>
static size_t reserve_n=0,peak_live=0;
static int reserve_mode=0;
#include "adapted/fuzz_insert_erase.cpp"
#include "adapted/fuzz_string.cpp"
int main(){size_t tests=0,files=0;std::string current;
 try{for(auto suite:{"fuzz_insert_erase","fuzz_string"}){
  std::vector<std::filesystem::path> paths;
  for(auto const& x:std::filesystem::directory_iterator(std::filesystem::path("ankerl/corpus")/suite))if(x.is_regular_file())paths.push_back(x.path());
  std::sort(paths.begin(),paths.end());
  for(auto const& path:paths){current=path.string();std::ifstream in(path,std::ios::binary);std::string data((std::istreambuf_iterator<char>(in)),{});fuzz::provider p(data.data(),data.size());
   reserve_mode=0;peak_live=0;
   if(std::string(suite)=="fuzz_insert_erase")insert_erase<std::unordered_map<uint64_t,uint64_t>>(p.copy());
   else do_string<std::unordered_map<std::string,std::string>>(p.copy());
   size_t peak=peak_live;
   for(reserve_mode=0;reserve_mode<3;++reserve_mode){reserve_n=reserve_mode==2?peak+peak/8+(peak%8!=0):peak;
    if(std::string(suite)=="fuzz_insert_erase")insert_erase<bflat::unordered_flat_map<uint64_t,uint64_t,dummy_hash>>(p.copy());
    else do_string<bflat::unordered_flat_map<std::string,std::string,ankerl::unordered_dense::hash<std::string>>>(p.copy());
    ++tests;
   }++files;
  }std::cout<<suite<<" files="<<paths.size()<<" PASS\n"<<std::flush;
 }std::cout<<"PASS files="<<files<<" bflat_cases="<<tests<<" reserve_modes=none,exact,headroom\n";
 }catch(std::exception const& e){std::cerr<<"FAIL file="<<current<<" reserve_mode="<<reserve_mode<<" "<<e.what()<<'\n';return 1;}}
