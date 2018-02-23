/* -*- c++ -*- */
/* 
 * Copyright 2018 <+YOU OR YOUR COMPANY+>.
 * 
 * This is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3, or (at your option)
 * any later version.
 * 
 * This software is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this software; see the file COPYING.  If not, write to
 * the Free Software Foundation, Inc., 51 Franklin Street,
 * Boston, MA 02110-1301, USA.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <gnuradio/io_signature.h>
#include "fddsm_mod_bcb_impl.h"

namespace gr {
  namespace lpwan {

    fddsm_mod_bcb::sptr
    fddsm_mod_bcb::make(int bps, int packet_len_bytes)
    {
      return gnuradio::get_initial_sptr
        (new fddsm_mod_bcb_impl(bps, packet_len_bytes));
    }

    /*
     * The private constructor
     */
    fddsm_mod_bcb_impl::fddsm_mod_bcb_impl(int bps, int packet_len_bytes)
      : gr::block("fddsm_mod_bcb",
              gr::io_signature::make(1, 1, sizeof(char)),
              gr::io_signature::make2(2, 2, sizeof(char), sizeof(gr_complex))),
              d_bps(bps),
              d_packet_len_bytes(packet_len_bytes),
              d_q_prev_state(0),
              d_s_prev({1, 1}),
              d_packet_len_symbols(d_packet_len_bytes * 8 / bps)
    {
      d_L = 1 << (bps-1); // 1 bit is encoded into the antenna information
      if(d_L > 8){ throw std::runtime_error("L > 8 is not supported.");}

      // Generate lookup tables for Aq and Vl
      d_antenna_indices = {{0,1}, {1,0}};
      if(d_L == 2)
      {
        auto omega = std::exp(gr_complex(0, M_PI/2));
        d_constellation_symbols = {{gr_complex(1,0), gr_complex(1,0)},
                                   {gr_complex(1,0)*omega, gr_complex(1,0)*omega},
                                   {gr_complex(-1,0)*omega, gr_complex(-1,0)*omega},
                                   {gr_complex(-1,0), gr_complex(-1,0)}};
        d_q = {0, 1, 1, 0};
      }
      else if (d_L == 4)
      {
        std::cout << "ATTENTION: For L=4, the symbols don't align nicely with byte boundaries." << std::endl;
        auto omega = std::exp(gr_complex(0, M_PI/4));
        d_constellation_symbols = {{gr_complex(1,0), gr_complex(1,0)},
                                   {gr_complex(1,0)*omega, gr_complex(1,0)*omega},
                                   {gr_complex(0,1)*omega, gr_complex(0,1)*omega},
                                   {gr_complex(0,1), gr_complex(0,1)},
                                   {gr_complex(0,-1), gr_complex(0,-1)},
                                   {gr_complex(0,-1)*omega, gr_complex(0,-1)*omega},
                                   {gr_complex(-1,0), gr_complex(-1,0)},
                                   {gr_complex(-1,0)*omega, gr_complex(-1,0)*omega}};
        d_q = {0, 1, 1, 0, 1, 0, 0, 1};
      }
      else if(d_L == 8) {
        auto omega = std::exp(gr_complex(0, M_PI / 4));
        auto u2 = 3;
        for (auto l = 0; l < 2*d_L; ++l)
        {
          if(l < d_L)
          {
            d_q.push_back(0);
            d_constellation_symbols.push_back(
                std::vector<gr_complex>(
                    {std::exp(gr_complex(0,2*M_PI*l/d_L)), std::exp(gr_complex(0,2*M_PI*u2*l/d_L))}
                )
            );
          }
          else
          {
            d_q.push_back(1);
            d_constellation_symbols.push_back(
                std::vector<gr_complex>(
                    {std::exp(gr_complex(0,2*M_PI*l/d_L))*omega, std::exp(gr_complex(0,2*M_PI*u2*l/d_L))*omega}
                )
            );
          }
        }
      }

      set_relative_rate(2.0/d_bps);
      set_output_multiple(d_packet_len_bytes * 8 / d_bps * 2);
    }

    /*
     * Our virtual destructor.
     */
    fddsm_mod_bcb_impl::~fddsm_mod_bcb_impl()
    {
    }

    void
    fddsm_mod_bcb_impl::forecast(int noutput_items, gr_vector_int &ninput_items_required)
    {
      ninput_items_required[0] = d_packet_len_bytes * 8; // bytes come in unpacked
    }

    int
    fddsm_mod_bcb_impl::general_work (int noutput_items,
                       gr_vector_int &ninput_items,
                       gr_vector_const_void_star &input_items,
                       gr_vector_void_star &output_items)
    {
      auto *bits_in = (const char *) input_items[0];
      auto *antenna_out = (char *) output_items[0];
      auto *symbol_out = (gr_complex *) output_items[1];

      auto nbits_in = ninput_items[0];

      //std::cout << "work(): bits_in/symbols_out " << nbits_in << "/" << nsym_out << std::endl;

      // this is the beginning of a frame, so reset s and q
      d_s_prev = {1, 1};
      d_q_prev_state = 0;

      for(auto i = 0; i < d_packet_len_symbols; ++i) // symbols means STBC matrices with two time slots each
      {
        // decimal index from input bit word
        auto idx = 0;
        for(auto n = 0; n < d_bps; ++n)
        {
          idx += (*bits_in) << (d_bps-1-n); // convert bps bits to decimal
          bits_in++;
        }

        // antenna indices
        auto q = d_q[idx];
        memcpy(antenna_out, &d_antenna_indices[d_q_prev_state^q][0], 2);
        antenna_out += 2;

        // symbol indices, multiply previous with current symbols
        // NOTE: This scheme results for the non-zero elements after multiplication of 2 (possibly permutated) diagonal matrices
        *(symbol_out) = d_s_prev[q] * d_constellation_symbols[idx][0];
        *(symbol_out+1) = d_s_prev[q^1] * d_constellation_symbols[idx][1];
        symbol_out += 2;

        /*(std::cout << "b0b1b2b3: " << int(*(bits_in-4)) << int(*(bits_in-3)) << int(*(bits_in-2)) << int(*(bits_in-1)) << "; qprev/q: " << int(d_q_prev_state) << "/" << int(q);
        std::cout << "; x0/x1: " << x0 << "/" << x1 << "; s0/s1: " << s0 << "/" << s1 << "; idx: " << idx << std::endl;*/

        d_q_prev_state = d_q_prev_state^q; // if q==0, the order is maintained, else it is switched
        d_s_prev = {*(symbol_out-2), *(symbol_out-1)};
      }

      consume_each(d_packet_len_bytes * 8);
      return 2 * d_packet_len_symbols;
    }

  } /* namespace lpwan */
} /* namespace gr */

