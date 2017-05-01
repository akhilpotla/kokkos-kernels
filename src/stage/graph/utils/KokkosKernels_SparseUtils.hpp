/*
//@HEADER
// ************************************************************************
//
//               KokkosKernels 0.9: Linear Algebra and Graph Kernels
//                 Copyright 2017 Sandia Corporation
//
// Under the terms of Contract DE-AC04-94AL85000 with Sandia Corporation,
// the U.S. Government retains certain rights in this software.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are
// met:
//
// 1. Redistributions of source code must retain the above copyright
// notice, this list of conditions and the following disclaimer.
//
// 2. Redistributions in binary form must reproduce the above copyright
// notice, this list of conditions and the following disclaimer in the
// documentation and/or other materials provided with the distribution.
//
// 3. Neither the name of the Corporation nor the names of the
// contributors may be used to endorse or promote products derived from
// this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY SANDIA CORPORATION "AS IS" AND ANY
// EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
// PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL SANDIA CORPORATION OR THE
// CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
// EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
// PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
// PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
// LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
// NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
// SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
//
// Questions? Contact Siva Rajamanickam (srajama@sandia.gov)
//
// ************************************************************************
//@HEADER
*/
#ifndef _KOKKOSKERNELS_SPARSEUTILS_HPP
#define _KOKKOSKERNELS_SPARSEUTILS_HPP
#include "Kokkos_Core.hpp"
#include "Kokkos_Atomic.hpp"
#include "impl/Kokkos_Timer.hpp"
#include "KokkosKernels_SimpleUtils.hpp"
#include "KokkosKernels_IOUtils.hpp"
#include "KokkosKernels_ExecSpaceUtils.hpp"
#include <vector>
//#include "KokkosKernels_Handle.hpp"
namespace KokkosKernels{

namespace Experimental{

namespace Util{

template <typename in_row_view_t,
          typename in_nnz_view_t,
          typename in_scalar_view_t,
          typename out_row_view_t,
          typename out_nnz_view_t,
          typename out_scalar_view_t,
          typename tempwork_row_view_t,
          typename MyExecSpace>
struct TransposeMatrix{

  struct CountTag{};
  struct FillTag{};

  typedef struct CountTag CountTag;
  typedef struct FillTag FillTag;

  typedef Kokkos::TeamPolicy<CountTag, MyExecSpace> team_count_policy_t ;
  typedef Kokkos::TeamPolicy<FillTag, MyExecSpace> team_fill_policy_t ;

  typedef Kokkos::TeamPolicy<CountTag, MyExecSpace, Kokkos::Schedule<Kokkos::Dynamic> > dynamic_team_count_policy_t ;
  typedef Kokkos::TeamPolicy<FillTag, MyExecSpace, Kokkos::Schedule<Kokkos::Dynamic> > dynamic_team_fill_policy_t ;


  typedef typename team_count_policy_t::member_type team_count_member_t ;
  typedef typename team_fill_policy_t::member_type team_fill_member_t ;

  typedef typename in_nnz_view_t::non_const_value_type nnz_lno_t;
  typedef typename in_row_view_t::non_const_value_type size_type;


  typename in_nnz_view_t::non_const_value_type num_rows;
  typename in_nnz_view_t::non_const_value_type num_cols;
  in_row_view_t xadj;
  in_nnz_view_t adj;
  in_scalar_view_t vals;
  out_row_view_t t_xadj; //allocated
  out_nnz_view_t t_adj;  //allocated
  out_scalar_view_t t_vals;  //allocated
  tempwork_row_view_t tmp_txadj;
  bool transpose_values;
  nnz_lno_t team_work_size;

  TransposeMatrix(
      nnz_lno_t num_rows_,
      nnz_lno_t num_cols_,
      in_row_view_t xadj_,
      in_nnz_view_t adj_,
      in_scalar_view_t vals_,
      out_row_view_t t_xadj_,
      out_nnz_view_t t_adj_,
      out_scalar_view_t t_vals_,
      tempwork_row_view_t tmp_txadj_,
      bool transpose_values_,
      nnz_lno_t team_row_work_size_):
        num_rows(num_rows_), num_cols(num_cols_),
        xadj(xadj_), adj(adj_), vals(vals_),
        t_xadj(t_xadj_),  t_adj(t_adj_), t_vals(t_vals_),
        tmp_txadj(tmp_txadj_), transpose_values(transpose_values_), team_work_size(team_row_work_size_) {}

  KOKKOS_INLINE_FUNCTION
  void operator()(const CountTag&, const team_count_member_t & teamMember) const {

    const nnz_lno_t team_row_begin = teamMember.league_rank() * team_work_size;
    const nnz_lno_t team_row_end = KOKKOSKERNELS_MACRO_MIN(team_row_begin + team_work_size, num_rows);
    //TODO we dont need to go over rows
    //just go over nonzeroes.
    Kokkos::parallel_for(Kokkos::TeamThreadRange(teamMember,team_row_begin,team_row_end), [&] (const nnz_lno_t& row_index) {
      const size_type col_begin = xadj[row_index];
      const size_type col_end = xadj[row_index + 1];
      const nnz_lno_t left_work = col_end - col_begin;
      Kokkos::parallel_for(
          Kokkos::ThreadVectorRange(teamMember, left_work),
          [&] (nnz_lno_t i) {
        const size_type adjind = i + col_begin;
        const nnz_lno_t colIndex = adj[adjind];
        Kokkos::atomic_fetch_add(&(t_xadj(colIndex)),1);
      });
    });
  }

  KOKKOS_INLINE_FUNCTION
  void operator()(const FillTag&, const team_fill_member_t & teamMember) const {
    const nnz_lno_t team_row_begin = teamMember.league_rank() * team_work_size;
    const nnz_lno_t team_row_end = KOKKOSKERNELS_MACRO_MIN(team_row_begin + team_work_size, num_rows);


    Kokkos::parallel_for(Kokkos::TeamThreadRange(teamMember,team_row_begin,team_row_end), [&] (const nnz_lno_t& row_index) {
    //const nnz_lno_t teamsize = teamMember.team_size();
    //for (nnz_lno_t row_index = team_row_begin + teamMember.team_rank(); row_index < team_row_end; row_index += teamsize){
      const size_type col_begin = xadj[row_index];
      const size_type col_end = xadj[row_index + 1];
      const nnz_lno_t left_work = col_end - col_begin;
      Kokkos::parallel_for(
          Kokkos::ThreadVectorRange(teamMember, left_work),
          [&] (nnz_lno_t i) {
        const size_type adjind = i + col_begin;
        const nnz_lno_t colIndex = adj[adjind];
        const size_type pos = Kokkos::atomic_fetch_add(&(tmp_txadj(colIndex)),1);

        t_adj(pos) = row_index;
        if (transpose_values){
          t_vals(pos) = vals[adjind];
        }

      });
    //}
    });
  }
};

/**
 * \brief function returns transpose of the given graph.
 * \param num_rows: num rows in input graph
 * \param num_cols: num cols in input graph
 * \param xadj: row pointers of the input graph
 * \param adj: column indices of the input graph
 * \param t_xadj: output, the row indices of the output graph. MUST BE INITIALIZED WITH ZEROES.
 * \param t_adj: output, column indices. No need for initializations.
 * \param vector_size: suggested vector size, optional. if -1, kernel will decide.
 * \param suggested_team_size: suggested team size, optional. if -1, kernel will decide.
 * \param team_work_chunk_size: suggested work size of a team, optional. if -1, kernel will decide.
 * \param use_dynamic_scheduling: whether to use dynamic scheduling. Default is true.
 */
template <typename in_row_view_t,
          typename in_nnz_view_t,
          typename out_row_view_t,
          typename out_nnz_view_t,
          typename tempwork_row_view_t,
          typename MyExecSpace>
inline void kk_transpose_graph(
    typename in_nnz_view_t::non_const_value_type num_rows,
    typename in_nnz_view_t::non_const_value_type num_cols,
    in_row_view_t xadj,
    in_nnz_view_t adj,
    out_row_view_t t_xadj, //pre-allocated -- initialized with 0
    out_nnz_view_t t_adj,  //pre-allocated -- no need for initialize
    int vector_size = -1,
    int suggested_team_size = -1,
    typename in_nnz_view_t::non_const_value_type team_work_chunk_size = -1,
    bool use_dynamic_scheduling = true
    ){

  //allocate some memory for work for row pointers
  tempwork_row_view_t tmp_row_view(Kokkos::ViewAllocateWithoutInitializing("tmp_row_view"), num_cols + 1);

  in_nnz_view_t tmp1;
  out_nnz_view_t tmp2;

  //create the functor for tranpose.
  typedef TransposeMatrix <
      in_row_view_t, in_nnz_view_t, in_nnz_view_t,
      out_row_view_t, out_nnz_view_t, out_nnz_view_t,
      tempwork_row_view_t, MyExecSpace>  TransposeFunctor_t;

  TransposeFunctor_t tm ( num_rows, num_cols, xadj, adj, tmp1,
                          t_xadj, t_adj, tmp2,
                          tmp_row_view,
                          false,
                          team_work_chunk_size);

  typedef typename TransposeFunctor_t::team_count_policy_t count_tp_t;
  typedef typename TransposeFunctor_t::team_fill_policy_t fill_tp_t;
  typedef typename TransposeFunctor_t::dynamic_team_count_policy_t d_count_tp_t;
  typedef typename TransposeFunctor_t::dynamic_team_fill_policy_t d_fill_tp_t;

  typename in_row_view_t::non_const_value_type nnz = adj.dimension_0();

  //set the vector size, if not suggested.
  if (vector_size == -1)
    vector_size = kk_get_suggested_vector_size(num_rows, nnz, kk_get_exec_space_type<MyExecSpace>());

  //set the team size, if not suggested.
  if (suggested_team_size == -1)
    suggested_team_size = kk_get_suggested_team_size(vector_size, kk_get_exec_space_type<MyExecSpace>());

  //set the chunk size, if not suggested.
  if (team_work_chunk_size == -1)
    team_work_chunk_size = suggested_team_size;



  if (use_dynamic_scheduling){
    Kokkos::parallel_for(  d_count_tp_t(num_rows  / team_work_chunk_size + 1 , suggested_team_size, vector_size), tm);
  }
  else {
    Kokkos::parallel_for(  count_tp_t(num_rows  / team_work_chunk_size + 1 , suggested_team_size, vector_size), tm);
  }
  MyExecSpace::fence();

  kk_exclusive_parallel_prefix_sum<out_row_view_t, MyExecSpace>(num_cols+1, t_xadj);
  MyExecSpace::fence();

  Kokkos::deep_copy(tmp_row_view, t_xadj);
  MyExecSpace::fence();


  if (use_dynamic_scheduling){
    Kokkos::parallel_for(  fill_tp_t(num_rows  / team_work_chunk_size + 1 , suggested_team_size, vector_size), tm);
  }
  else {
    Kokkos::parallel_for(  d_fill_tp_t(num_rows  / team_work_chunk_size + 1 , suggested_team_size, vector_size), tm);
  }
  MyExecSpace::fence();
}

template <typename forward_map_type, typename reverse_map_type>
struct Fill_Reverse_Scale_Functor{

  struct CountTag{};
  struct FillTag{};

  typedef struct CountTag CountTag;
  typedef struct FillTag FillTag;


  typedef typename forward_map_type::value_type forward_type;
  typedef typename reverse_map_type::value_type reverse_type;
  forward_map_type forward_map;
  reverse_map_type reverse_map_xadj;
  reverse_map_type reverse_map_adj;

  const reverse_type multiply_shift_for_scale;
  const reverse_type division_shift_for_bucket;


  Fill_Reverse_Scale_Functor(
      forward_map_type forward_map_,
      reverse_map_type reverse_map_xadj_,
      reverse_map_type reverse_map_adj_,
      reverse_type multiply_shift_for_scale_,
      reverse_type division_shift_for_bucket_):
        forward_map(forward_map_), reverse_map_xadj(reverse_map_xadj_), reverse_map_adj(reverse_map_adj_),
        multiply_shift_for_scale(multiply_shift_for_scale_),
        division_shift_for_bucket(division_shift_for_bucket_){}

  KOKKOS_INLINE_FUNCTION
  void operator()(const CountTag&, const size_t &ii) const {
    forward_type fm = forward_map[ii];
    fm = fm << multiply_shift_for_scale;
    fm += ii >> division_shift_for_bucket;
    Kokkos::atomic_fetch_add( &(reverse_map_xadj(fm)), 1);
  }

  KOKKOS_INLINE_FUNCTION
  void operator()(const FillTag&, const size_t &ii) const {
    forward_type fm = forward_map[ii];

    fm = fm << multiply_shift_for_scale;
    fm += ii >> division_shift_for_bucket;
    const reverse_type future_index = Kokkos::atomic_fetch_add( &(reverse_map_xadj(fm )), 1);
    reverse_map_adj(future_index) = ii;
  }
};


template <typename from_view_t, typename to_view_t>
struct StridedCopy1{
  const from_view_t from;
  to_view_t to;
  const size_t stride;
  StridedCopy1(
      const from_view_t from_,
      to_view_t to_,
      size_t stride_):from(from_), to (to_), stride(stride_){}


  KOKKOS_INLINE_FUNCTION
  void operator()(const size_t &ii) const {
    to[ii] = from[(ii) * stride];
  }
};

template <typename forward_map_type, typename reverse_map_type>
struct Reverse_Map_Functor{

  struct CountTag{};
  struct FillTag{};

  typedef struct CountTag CountTag;
  typedef struct FillTag FillTag;


  typedef typename forward_map_type::value_type forward_type;
  typedef typename reverse_map_type::value_type reverse_type;
  forward_map_type forward_map;
  reverse_map_type reverse_map_xadj;
  reverse_map_type reverse_map_adj;


  Reverse_Map_Functor(
      forward_map_type forward_map_,
      reverse_map_type reverse_map_xadj_,
      reverse_map_type reverse_map_adj_):
        forward_map(forward_map_), reverse_map_xadj(reverse_map_xadj_), reverse_map_adj(reverse_map_adj_){}

  KOKKOS_INLINE_FUNCTION
  void operator()(const CountTag&, const size_t &ii) const {
    forward_type fm = forward_map[ii];
    Kokkos::atomic_fetch_add( &(reverse_map_xadj(fm)), 1);
  }

  KOKKOS_INLINE_FUNCTION
  void operator()(const FillTag&, const size_t &ii) const {
    forward_type c = forward_map[ii];
    const reverse_type future_index = Kokkos::atomic_fetch_add( &(reverse_map_xadj(c)), 1);
    reverse_map_adj(future_index) = ii;
  }
};


/**
 * \brief Utility function to obtain a reverse map given a map.
 * Input is a map with the number of elements within the map.
 * forward_map[c] = i, where c is a forward element and forward_map has a size of num_forward_elements.
 * i is the value that c is mapped in the forward map, and the range of that is num_reverse_elements.
 * Output is the reverse_map_xadj and reverse_map_adj such that,
 * all c, forward_map[c] = i, will appear in  reverse_map_adj[ reverse_map_xadj[i]: reverse_map_xadj[i+1])
 * \param: num_forward_elements: the number of elements in the forward map, the size of the forward map.
 * \param: num_reverse_elements: the number of elements that forward map is mapped to. It is the value of max i.
 * \param: forward_map: input forward_map, where forward_map[c] = i.
 * \param: reverse_map_xadj: reverse map xadj, that is it will hold the beginning and
 * end indices on reverse_map_adj such that all values mapped to i will be [ reverse_map_xadj[i]: reverse_map_xadj[i+1])
 * its size will be num_reverse_elements + 1. NO NEED TO INITIALIZE.
 * \param: reverse_map_adj: reverse map adj, holds the values of reverse maps. Its size is num_forward_elements.
 *
 */
template <typename forward_array_type, typename reverse_array_type, typename MyExecSpace>
void kk_create_reverse_map(
    const typename reverse_array_type::value_type &num_forward_elements, //num_vertices
    const typename forward_array_type::value_type &num_reverse_elements, //num_colors

    const forward_array_type &forward_map, //vertex to colors
    const reverse_array_type &reverse_map_xadj, // colors to vertex xadj
    const reverse_array_type &reverse_map_adj){ //colros to vertex adj

  typedef typename reverse_array_type::value_type lno_t;
  typedef typename forward_array_type::value_type reverse_lno_t;

  const lno_t  MINIMUM_TO_ATOMIC = 128;

  //typedef Kokkos::TeamPolicy<CountTag, MyExecSpace> team_count_policy_t ;
  //typedef Kokkos::TeamPolicy<FillTag, MyExecSpace> team_fill_policy_t ;

  typedef Kokkos::RangePolicy<MyExecSpace> my_exec_space;

  //IF There are very few reverse elements, atomics are likely to create contention.
  if (num_reverse_elements < MINIMUM_TO_ATOMIC){
    const lno_t scale_size = 1024;
    const lno_t multiply_shift_for_scale = 10;

    //there will be 1024 buckets
    const lno_t division_shift_for_bucket =
          lno_t (ceil(log(double (num_forward_elements) / scale_size)/log(2)));

    //coloring indices are base-1. we end up using not using element 1.
    const reverse_lno_t tmp_reverse_size =
        (num_reverse_elements + 1) << multiply_shift_for_scale;

    typename reverse_array_type::non_const_type
        tmp_color_xadj ("TMP_REVERSE_XADJ", tmp_reverse_size + 1);

    typedef Fill_Reverse_Scale_Functor<forward_array_type, reverse_array_type> frsf;
    typedef typename frsf::CountTag cnt_tag;
    typedef typename frsf::FillTag fill_tag;
    typedef Kokkos::RangePolicy<cnt_tag, MyExecSpace> my_cnt_exec_space;
    typedef Kokkos::RangePolicy<fill_tag, MyExecSpace> my_fill_exec_space;

    frsf frm (forward_map, tmp_color_xadj, reverse_map_adj,
            multiply_shift_for_scale, division_shift_for_bucket);

    Kokkos::parallel_for (my_cnt_exec_space (0, num_forward_elements) , frm);
    MyExecSpace::fence();


    //kk_inclusive_parallel_prefix_sum<reverse_array_type, MyExecSpace>(tmp_reverse_size + 1, tmp_color_xadj);
    kk_exclusive_parallel_prefix_sum<reverse_array_type, MyExecSpace>
      (tmp_reverse_size + 1, tmp_color_xadj);
    MyExecSpace::fence();

    Kokkos::parallel_for (
        my_exec_space (0, num_reverse_elements + 1) ,
        StridedCopy1<reverse_array_type, reverse_array_type>
          (tmp_color_xadj, reverse_map_xadj, scale_size));
    MyExecSpace::fence();
    Kokkos::parallel_for (my_fill_exec_space (0, num_forward_elements) , frm);
    MyExecSpace::fence();
  }
  else
  //atomic implementation.
  {
    reverse_array_type tmp_color_xadj ("TMP_REVERSE_XADJ", num_reverse_elements + 1);

    typedef Reverse_Map_Functor<forward_array_type, reverse_array_type> rmp_functor_type;
    typedef typename rmp_functor_type::CountTag cnt_tag;
    typedef typename rmp_functor_type::FillTag fill_tag;
    typedef Kokkos::RangePolicy<cnt_tag, MyExecSpace> my_cnt_exec_space;
    typedef Kokkos::RangePolicy<fill_tag, MyExecSpace> my_fill_exec_space;

    rmp_functor_type frm (forward_map, tmp_color_xadj, reverse_map_adj);

    Kokkos::parallel_for (my_cnt_exec_space (0, num_forward_elements) , frm);
    MyExecSpace::fence();

    //kk_inclusive_parallel_prefix_sum<reverse_array_type, MyExecSpace>(num_reverse_elements + 1, reverse_map_xadj);
    kk_exclusive_parallel_prefix_sum<reverse_array_type, MyExecSpace>
      (num_reverse_elements + 1, tmp_color_xadj);
    MyExecSpace::fence();

    Kokkos::deep_copy (reverse_map_xadj, tmp_color_xadj);
    MyExecSpace::fence();

    Kokkos::parallel_for (my_fill_exec_space (0, num_forward_elements) , frm);
    MyExecSpace::fence();
  }
}

template <typename in_row_view_t, typename in_nnz_view_t,  typename in_color_view_t,
          typename team_member>
struct ColorChecker{



  typedef typename in_row_view_t::value_type size_type;
  typedef typename in_nnz_view_t::value_type lno_t;
  typedef typename in_color_view_t::value_type color_t;
  in_row_view_t xadj;
  in_nnz_view_t adj;
  in_color_view_t color_view;
  lno_t team_row_chunk_size;
  lno_t num_rows;


  ColorChecker(
      lno_t num_rows_,
      in_row_view_t xadj_,
      in_nnz_view_t adj_,
      in_color_view_t color_view_,
      lno_t chunk_size):
        num_rows(num_rows_),
        xadj(xadj_), adj(adj_), color_view(color_view_),
        team_row_chunk_size(chunk_size){}

  KOKKOS_INLINE_FUNCTION
  void operator()(const team_member & teamMember, size_t &num_conflicts) const {
    //get the range of rows for team.
    const lno_t team_row_begin = teamMember.league_rank() * team_row_chunk_size;
    const lno_t team_row_end = KOKKOSKERNELS_MACRO_MIN(team_row_begin + team_row_chunk_size, num_rows);

    size_t nf = 0;
    Kokkos::parallel_reduce(Kokkos::TeamThreadRange(teamMember, team_row_begin, team_row_end), [&] (const lno_t& row_index, size_t &team_num_conf)
    {

      color_t my_color = color_view(row_index);
      const size_type col_begin = xadj[row_index];
      const size_type col_end = xadj[row_index + 1];
      const lno_t left_work = col_end - col_begin;

      size_t conf1= 0;
      Kokkos::parallel_reduce(
          Kokkos::ThreadVectorRange(teamMember, left_work),
          [&] (lno_t i, size_t & valueToUpdate) {
        const size_type adjind = i + col_begin;
        const lno_t colIndex = adj[adjind];
        if (colIndex != row_index){
          color_t second_color = color_view(colIndex);
          if (second_color == my_color)
            valueToUpdate += 1;
        }
      },
      conf1);
      team_num_conf += conf1;
    }, nf);
    num_conflicts += nf;
  }
};

/**
 * \brief given a graph and a coloring function returns true or false if distance-1 coloring is valid or not.
 * \param num_rows: num rows in input graph
 * \param num_cols: num cols in input graph
 * \param xadj: row pointers of the input graph
 * \param adj: column indices of the input graph
 * \param t_xadj: output, the row indices of the output graph. MUST BE INITIALIZED WITH ZEROES.

 * \param vector_size: suggested vector size, optional. if -1, kernel will decide.
 * \param suggested_team_size: suggested team size, optional. if -1, kernel will decide.
 * \param team_work_chunk_size: suggested work size of a team, optional. if -1, kernel will decide.
 * \param use_dynamic_scheduling: whether to use dynamic scheduling. Default is true.
 */
template <typename in_row_view_t,
          typename in_nnz_view_t,
          typename in_color_view_t,
          typename MyExecSpace>
inline size_t kk_is_d1_coloring_valid(
    typename in_nnz_view_t::non_const_value_type num_rows,
    typename in_nnz_view_t::non_const_value_type num_cols,
    in_row_view_t xadj,
    in_nnz_view_t adj,
    in_color_view_t v_colors
    ){
  KokkosKernels::Experimental::Util::ExecSpaceType my_exec_space = KokkosKernels::Experimental::Util::kk_get_exec_space_type<MyExecSpace>();
  int vector_size = KokkosKernels::Experimental::Util::kk_get_suggested_vector_size(num_rows, adj.dimension_0(), my_exec_space);
  int suggested_team_size = KokkosKernels::Experimental::Util::kk_get_suggested_team_size(vector_size, my_exec_space);;
  typename in_nnz_view_t::non_const_value_type team_work_chunk_size = suggested_team_size;
  typedef Kokkos::TeamPolicy<MyExecSpace, Kokkos::Schedule<Kokkos::Dynamic> > dynamic_team_policy ;
  typedef typename dynamic_team_policy::member_type team_member_t ;

  struct ColorChecker <in_row_view_t, in_nnz_view_t, in_color_view_t, team_member_t>  cc(num_rows, xadj, adj, v_colors, team_work_chunk_size);
  size_t num_conf = 0;
  Kokkos::parallel_reduce( dynamic_team_policy(num_rows / team_work_chunk_size + 1 ,
      suggested_team_size, vector_size), cc, num_conf);

  MyExecSpace::fence();
  return num_conf;
}


template <typename lno_view_t,
          typename lno_nnz_view_t,
          typename scalar_view_t,

          typename out_nnz_view_t,
          typename out_scalar_view_t,
          typename MyExecSpace>
void kk_sort_graph(
    lno_view_t in_xadj,
    lno_nnz_view_t in_adj,
    scalar_view_t in_vals,

    out_nnz_view_t out_adj,
    out_scalar_view_t out_vals){
  KokkosKernels::Experimental::Util::ExecSpaceType exec = KokkosKernels::Experimental::Util::kk_get_exec_space_type<MyExecSpace>();

  if (exec == KokkosKernels::Experimental::Util::Exec_CUDA){
    typename lno_view_t::HostMirror hr = Kokkos::create_mirror_view (in_xadj);
    Kokkos::deep_copy (hr, in_xadj);
    typename lno_nnz_view_t::HostMirror he = Kokkos::create_mirror_view (in_adj);
    Kokkos::deep_copy (he, in_adj);
    typename scalar_view_t::HostMirror hv = Kokkos::create_mirror_view (in_vals);
    Kokkos::deep_copy (hv, in_vals);

    typename lno_nnz_view_t::HostMirror heo = Kokkos::create_mirror_view (out_adj);
    typename scalar_view_t::HostMirror hvo = Kokkos::create_mirror_view (out_vals);


    typedef typename lno_view_t::non_const_value_type size_type;
    typedef typename lno_nnz_view_t::non_const_value_type lno_t;
    typedef typename scalar_view_t::non_const_value_type scalar_t;

    lno_t nrows = in_xadj.dimension_0() - 1;
    std::vector <KokkosKernels::Experimental::Util::Edge<lno_t, scalar_t> > edges(in_adj.dimension_0());

    size_type row_size = 0;
    for (lno_t i = 0; i < nrows; ++i){
      for (size_type j = hr(i); j < hr(i + 1); ++j){
        edges[row_size].src = i;
        edges[row_size].dst = he(j);
        edges[row_size++].ew = hv(j);
      }
    }
    std::sort (edges.begin(), edges.begin() + row_size);
    size_type ne = in_adj.dimension_0();
    for(size_type i = 0; i < ne; ++i){
      out_adj(i) = edges[i].dst;
      out_vals(i) = edges[i].ew;
    }


    Kokkos::deep_copy (out_adj, heo);
    Kokkos::deep_copy (out_vals, hvo);
  }
  else {


    typedef typename lno_view_t::non_const_value_type size_type;
    typedef typename lno_nnz_view_t::non_const_value_type lno_t;
    typedef typename scalar_view_t::non_const_value_type scalar_t;

    lno_t nrows = in_xadj.dimension_0() - 1;
    std::vector <KokkosKernels::Experimental::Util::Edge<lno_t, scalar_t> > edges(in_adj.dimension_0());

    size_type row_size = 0;
    for (lno_t i = 0; i < nrows; ++i){
      for (size_type j = in_xadj(i); j < in_xadj(i + 1); ++j){
        edges[row_size].src = i;
        edges[row_size].dst = in_adj(j);
        edges[row_size++].ew = in_vals(j);
      }
    }
    std::sort (edges.begin(), edges.begin() + row_size);
    size_type ne = in_adj.dimension_0();
    for(size_type i = 0; i < ne; ++i){
      out_adj(i) = edges[i].dst;
      out_vals(i) = edges[i].ew;
    }



  }
}

/*
template <typename in_row_view_t,
          typename in_nnz_view_t,
          typename out_nnz_view_t,
          typename MyExecSpace>
struct IncidenceMatrix{

  struct FillTag{};

  typedef struct FillTag FillTag;

  typedef Kokkos::TeamPolicy<FillTag, MyExecSpace> team_fill_policy_t ;
  typedef Kokkos::TeamPolicy<FillTag, MyExecSpace, Kokkos::Schedule<Kokkos::Dynamic> > dynamic_team_fill_policy_t ;
  typedef typename team_fill_policy_t::member_type team_fill_member_t ;

  typedef typename in_nnz_view_t::non_const_value_type nnz_lno_t;
  typedef typename in_row_view_t::non_const_value_type size_type;


  typename in_nnz_view_t::non_const_value_type num_rows;
  in_row_view_t xadj;
  in_nnz_view_t adj;
  out_nnz_view_t t_adj;  //allocated
  typename in_row_view_t::non_const_type tmp_txadj;
  nnz_lno_t team_work_size;

  IncidenceMatrix(
      nnz_lno_t num_rows_,
      in_row_view_t xadj_,
      in_nnz_view_t adj_,
      out_nnz_view_t t_adj_,
      typename in_row_view_t::non_const_type tmp_txadj_,
      nnz_lno_t team_row_work_size_):
        num_rows(num_rows_),
        xadj(xadj_), adj(adj_),
        t_adj(t_adj_),
        tmp_txadj(tmp_txadj_), team_work_size(team_row_work_size_) {}


  KOKKOS_INLINE_FUNCTION
  void operator()(const FillTag&, const team_fill_member_t & teamMember) const {
    const nnz_lno_t team_row_begin = teamMember.league_rank() * team_work_size;
    const nnz_lno_t team_row_end = KOKKOSKERNELS_MACRO_MIN(team_row_begin + team_work_size, num_rows);


    Kokkos::parallel_for(Kokkos::TeamThreadRange(teamMember,team_row_begin,team_row_end), [&] (const nnz_lno_t& row_index) {
      const size_type col_begin = xadj[row_index];
      const size_type col_end = xadj[row_index + 1];
      const nnz_lno_t left_work = col_end - col_begin;
      Kokkos::parallel_for(
          Kokkos::ThreadVectorRange(teamMember, left_work),
          [&] (nnz_lno_t i) {
        const size_type adjind = i + col_begin;
        const nnz_lno_t colIndex = adj[adjind];
        if (row_index < colIndex){

          const size_type pos = Kokkos::atomic_fetch_add(&(tmp_txadj(colIndex)),1);
          t_adj(adjind) = adjind;
          t_adj(pos) = adjind;
        }
      });
    //}
    });
  }
};
*/
/**
 * \brief function returns transpose of the given graph.
 * \param num_rows: num rows in input graph
 * \param num_cols: num cols in input graph
 * \param xadj: row pointers of the input graph
 * \param adj: column indices of the input graph
 * \param t_xadj: output, the row indices of the output graph. MUST BE INITIALIZED WITH ZEROES.
 * \param t_adj: output, column indices. No need for initializations.
 * \param vector_size: suggested vector size, optional. if -1, kernel will decide.
 * \param suggested_team_size: suggested team size, optional. if -1, kernel will decide.
 * \param team_work_chunk_size: suggested work size of a team, optional. if -1, kernel will decide.
 * \param use_dynamic_scheduling: whether to use dynamic scheduling. Default is true.
 */
/*
template <typename in_row_view_t,
          typename in_nnz_view_t,
          typename out_nnz_view_t,
          typename MyExecSpace>
inline void kk_create_incidence_matrix(
    typename in_nnz_view_t::non_const_value_type num_rows,
    in_row_view_t xadj,
    in_nnz_view_t adj,
    out_nnz_view_t i_adj,  //pre-allocated -- no need for initialize -- size is same as adj
    int vector_size = -1,
    int suggested_team_size = -1,
    typename in_nnz_view_t::non_const_value_type team_work_chunk_size = -1,
    bool use_dynamic_scheduling = true
    ){


  typedef typename in_row_view_t::non_const_type tmp_row_view_t;
  //allocate some memory for work for row pointers
  tmp_row_view_t tmp_row_view(Kokkos::ViewAllocateWithoutInitializing("tmp_row_view"), num_rows + 1);

  Kokkos::deep_copy(tmp_row_view, xadj);

  in_nnz_view_t tmp1;
  out_nnz_view_t tmp2;

  //create the functor for tranpose.
  typedef IncidenceMatrix <
      in_row_view_t, in_nnz_view_t, in_nnz_view_t,
      out_nnz_view_t, MyExecSpace>  IncidenceMatrix_Functor_t;

  IncidenceMatrix_Functor_t tm ( num_rows, xadj, adj,
                                t_adj, tmp_row_view,
                                false,
                                team_work_chunk_size);


  typedef typename IncidenceMatrix_Functor_t::team_fill_policy_t fill_tp_t;
  typedef typename IncidenceMatrix_Functor_t::dynamic_team_fill_policy_t d_fill_tp_t;

  typename in_row_view_t::non_const_value_type nnz = adj.dimension_0();

  //set the vector size, if not suggested.
  if (vector_size == -1)
    vector_size = kk_get_suggested_vector_size(num_rows, nnz, kk_get_exec_space_type<MyExecSpace>());

  //set the team size, if not suggested.
  if (suggested_team_size == -1)
    suggested_team_size = kk_get_suggested_team_size(vector_size, kk_get_exec_space_type<MyExecSpace>());

  //set the chunk size, if not suggested.
  if (team_work_chunk_size == -1)
    team_work_chunk_size = suggested_team_size;



  if (use_dynamic_scheduling){
    Kokkos::parallel_for(  fill_tp_t(num_rows  / team_work_chunk_size + 1 , suggested_team_size, vector_size), tm);
  }
  else {
    Kokkos::parallel_for(  d_fill_tp_t(num_rows  / team_work_chunk_size + 1 , suggested_team_size, vector_size), tm);
  }
  MyExecSpace::fence();

}
*/





template <typename size_type, typename lno_t, typename ExecutionSpace>
void kk_get_lower_triangle_count(
    const lno_t nv,
    const size_type *in_xadj,
    const lno_t *in_adj,
    size_type *out_xadj,
    const lno_t *new_indices = NULL
    ){
  for (lno_t i = 0; i < nv; ++i){
    lno_t row_index = i;

    if (new_indices) row_index = new_indices[i];

    out_xadj[i] = 0;
    size_type begin = in_xadj[i];
    lno_t rowsize = in_xadj[i + 1] - begin;

    for (lno_t j = 0; j < rowsize; ++j){
      lno_t col = in_adj[j + begin];
      lno_t col_index = col;
      if (new_indices) col_index = new_indices[col];

      if (row_index > col_index){
        ++out_xadj[i];
      }
    }
  }
}



template <typename size_type, typename lno_t>
void kk_sort_by_row_size_sequential(
    const lno_t nv,
    const size_type *in_xadj,
    lno_t *new_indices){


  std::vector<lno_t> begins (nv);
  std::vector<lno_t> nexts (nv);
  for (lno_t i = 0; i < nv; ++i){
    nexts[i] = begins[i] = -1;
  }



  for (lno_t i = 0; i < nv; ++i){
    lno_t row_size = in_xadj[i+1] - in_xadj[i];
    nexts [i] = begins[row_size];
    begins[row_size] = i;
  }
  lno_t new_index = nv;
  for (lno_t i = 0; i < nv ; ++i){
    lno_t row = begins[i];
    while (row != -1){
      new_indices[row] = --new_index;
      row = nexts[row];
    }
  }
}

template <typename size_type, typename lno_t, typename ExecutionSpace>
void kk_sort_by_row_size_parallel(
    const lno_t nv,
    const size_type *in_xadj,
    lno_t *new_indices){

  typedef Kokkos::RangePolicy<ExecutionSpace> my_exec_space;


  Kokkos::View <lno_t *, ExecutionSpace> begins (Kokkos::ViewAllocateWithoutInitializing("begins"), nv);
  lno_t * p_begins = begins.data();
  Kokkos::View <lno_t *, ExecutionSpace> nexts (Kokkos::ViewAllocateWithoutInitializing("begins"), nv);
  Kokkos::View <lno_t *, ExecutionSpace> num_elements ("num_elements", nv);
  lno_t * p_num_elements = num_elements.data();
  const lno_t increment = 1;

  Kokkos::deep_copy(begins, -1);

  Kokkos::parallel_for( my_exec_space(0, nv),
      KOKKOS_LAMBDA(const lno_t& row) {
        lno_t row_size = in_xadj[row+1] - in_xadj[row];
        lno_t hashbeginning = Kokkos::atomic_exchange(p_begins+row_size, row);
        Kokkos::atomic_add(p_num_elements + row_size, increment);
        nexts [row] = hashbeginning;
      });

  kk_exclusive_parallel_prefix_sum< Kokkos::View <lno_t *, ExecutionSpace>, ExecutionSpace> (nv, num_elements);

  const lno_t end_linked_list = -1;
  Kokkos::parallel_for( my_exec_space(0, nv),
      KOKKOS_LAMBDA(const lno_t& i) {
    lno_t row = p_begins[i];
    lno_t new_index = nv - 1 - p_num_elements[i];
    while (row != end_linked_list){
      new_indices[row] = --new_index;
      row = nexts[row];
    }
  });
}
template <typename size_type, typename lno_t, typename ExecutionSpace>
void kk_sort_by_row_size(
    const lno_t nv,
    const size_type *in_xadj,
    lno_t *new_indices){
  //kk_sort_by_row_size_sequential(nv, in_xadj, new_indices);
  Kokkos::Impl::Timer timer1;
  kk_sort_by_row_size_parallel<size_type, lno_t, ExecutionSpace>(nv, in_xadj, new_indices);
  double sort_time = timer1.seconds();
  std::cout << "sort time:" << sort_time<< std::endl;

}
template <typename size_type, typename lno_t, typename scalar_t, typename ExecutionSpace>
void kk_get_lower_triangle_fill(
    lno_t nv,
    const size_type *in_xadj,
    const lno_t *in_adj,
    const scalar_t *in_vals,
    const size_type *out_xadj,
    lno_t *out_adj,
    scalar_t *out_vals,
    const lno_t *new_indices = NULL
    ){
  for (lno_t i = 0; i < nv; ++i){
    lno_t row_index = i;

    if (new_indices) row_index = new_indices[i];
    size_type write_index = out_xadj[i];
    size_type begin = in_xadj[i];
    lno_t rowsize = in_xadj[i + 1] - begin;
    for (lno_t j = 0; j < rowsize; ++j){
      lno_t col = in_adj[j + begin];
      lno_t col_index = col;
      if (new_indices) col_index = new_indices[col];

      if (row_index > col_index){
        if (in_vals != NULL && out_vals != NULL){
          out_vals[write_index] = in_vals[j + begin];
        }
        out_adj[write_index++] = col;
      }
    }
  }
}

template <typename crstmat_t>
crstmat_t kk_get_lower_crs_matrix(crstmat_t in_crs_matrix,
    typename crstmat_t::index_type::value_type *new_indices = NULL){

  typedef typename crstmat_t::execution_space exec_space;
  typedef typename crstmat_t::StaticCrsGraphType graph_t;
  typedef typename crstmat_t::row_map_type::non_const_type row_map_view_t;
  typedef typename crstmat_t::index_type::non_const_type   cols_view_t;
  typedef typename crstmat_t::values_type::non_const_type values_view_t;
  typedef typename crstmat_t::row_map_type::const_type const_row_map_view_t;
  typedef typename crstmat_t::index_type::const_type   const_cols_view_t;
  typedef typename crstmat_t::values_type::const_type const_values_view_t;

  typedef typename row_map_view_t::non_const_value_type size_type;
  typedef typename cols_view_t::non_const_value_type lno_t;
  typedef typename values_view_t::non_const_value_type scalar_t;


  lno_t nr = in_crs_matrix.numRows();
  const scalar_t *vals = in_crs_matrix.values.data();
  const size_type *rowmap = in_crs_matrix.graph.row_map.data();
  const lno_t *entries= in_crs_matrix.graph.entries.data();

  row_map_view_t new_row_map ("LL", nr + 1);
  KokkosKernels::Experimental::Util::kk_get_lower_triangle_count
  <size_type, lno_t, exec_space> (nr, rowmap, entries, new_row_map.data(), new_indices);

  size_type total = 0;
  for (lno_t i = 0; i < nr; ++i){
    total += new_row_map(i);
  }

  KokkosKernels::Experimental::Util::kk_exclusive_parallel_prefix_sum
  <row_map_view_t, exec_space>(nr + 1, new_row_map);
  exec_space::fence();

  auto ll_size = Kokkos::subview(new_row_map, nr);
  auto h_ll_size = Kokkos::create_mirror_view (ll_size);
  Kokkos::deep_copy (h_ll_size, ll_size);
  size_type ll_nnz_size = h_ll_size();

  cols_view_t new_entries ("LL", ll_nnz_size);
  values_view_t new_values ("LL", ll_nnz_size);

  KokkosKernels::Experimental::Util::kk_get_lower_triangle_fill
  <size_type, lno_t, scalar_t, exec_space> (
      nr, rowmap, entries, vals, new_row_map.data(),
      new_entries.data(), new_values.data(),new_indices);

  graph_t g (new_entries, new_row_map);
  crstmat_t new_ll_mtx("lower triangle", in_crs_matrix.numCols(), new_values, g);
  return new_ll_mtx;
}

template <typename graph_t>
graph_t kk_get_lower_crs_graph(graph_t in_crs_matrix,
    typename graph_t::data_type *new_indices = NULL){

  typedef typename graph_t::execution_space exec_space;

  typedef typename graph_t::row_map_type::non_const_type row_map_view_t;
  typedef typename graph_t::entries_type::non_const_type   cols_view_t;

  typedef typename graph_t::row_map_type::const_type const_row_map_view_t;
  typedef typename graph_t::entries_type::const_type   const_cols_view_t;


  typedef typename row_map_view_t::non_const_value_type size_type;
  typedef typename cols_view_t::non_const_value_type lno_t;



  lno_t nr = in_crs_matrix.numRows();
  const size_type *rowmap = in_crs_matrix.row_map.data();
  const lno_t *entries= in_crs_matrix.entries.data();

  row_map_view_t new_row_map ("LL", nr + 1);
  KokkosKernels::Experimental::Util::kk_get_lower_triangle_count
  <size_type, lno_t, exec_space> (nr, rowmap, entries, new_row_map.data(), new_indices);

  size_type total = 0;
  for (int i = 0; i < nr; ++i){
    total += new_row_map(i);
  }

  KokkosKernels::Experimental::Util::kk_exclusive_parallel_prefix_sum
  <row_map_view_t, exec_space>(nr + 1, new_row_map);
  exec_space::fence();

  auto ll_size = Kokkos::subview(new_row_map, nr);
  auto h_ll_size = Kokkos::create_mirror_view (ll_size);
  Kokkos::deep_copy (h_ll_size, ll_size);
  size_type ll_nnz_size = h_ll_size();

  cols_view_t new_entries ("LL", ll_nnz_size);


  KokkosKernels::Experimental::Util::kk_get_lower_triangle_fill
  <size_type, lno_t, lno_t, exec_space> (
      nr, rowmap, entries, NULL, new_row_map.data(),
      new_entries.data(), NULL,new_indices);

  graph_t g (new_entries, new_row_map);

  return g;
}

}
}
}
#endif
