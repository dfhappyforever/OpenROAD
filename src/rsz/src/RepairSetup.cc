/////////////////////////////////////////////////////////////////////////////
//
// Copyright (c) 2019, The Regents of the University of California
// All rights reserved.
//
// BSD 3-Clause License
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
// * Redistributions of source code must retain the above copyright notice, this
//   list of conditions and the following disclaimer.
//
// * Redistributions in binary form must reproduce the above copyright notice,
//   this list of conditions and the following disclaimer in the documentation
//   and/or other materials provided with the distribution.
//
// * Neither the name of the copyright holder nor the names of its
//   contributors may be used to endorse or promote products derived from
//   this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.
//
///////////////////////////////////////////////////////////////////////////////

#include "RepairSetup.hh"
#include "rsz/Resizer.hh"

#include "utl/Logger.h"
#include "db_sta/dbNetwork.hh"

#include "sta/Units.hh"
#include "sta/Liberty.hh"
#include "sta/TimingArc.hh"
#include "sta/Graph.hh"
#include "sta/DcalcAnalysisPt.hh"
#include "sta/GraphDelayCalc.hh"
#include "sta/Parasitics.hh"
#include "sta/Sdc.hh"
#include "sta/InputDrive.hh"
#include "sta/Corner.hh"
#include "sta/PathVertex.hh"
#include "sta/PathRef.hh"
#include "sta/PathExpanded.hh"
#include "sta/Fuzzy.hh"

namespace rsz {

using std::abs;
using std::min;
using std::max;
using std::string;
using std::vector;
using std::map;
using std::pair;

using utl::RSZ;

using sta::VertexOutEdgeIterator;
using sta::Edge;
using sta::Clock;
using sta::PathExpanded;
using sta::INF;
using sta::fuzzyEqual;
using sta::fuzzyLess;
using sta::fuzzyLessEqual;
using sta::fuzzyGreater;
using sta::fuzzyGreaterEqual;
using sta::Unit;
using sta::Corners;
using sta::InputDrive;

RepairSetup::RepairSetup(Resizer *resizer) :
  StaState(),
  logger_(nullptr),
  sta_(nullptr),
  db_network_(nullptr),
  resizer_(resizer),
  corner_(nullptr),
  drvr_port_(nullptr),
  resize_count_(0),
  inserted_buffer_count_(0),
  rebuffer_net_count_(0),
  min_(MinMax::min()),
  max_(MinMax::max())
{
}

void
RepairSetup::init()
{
  logger_ = resizer_->logger_;
  sta_ = resizer_->sta_;
  db_network_ = resizer_->db_network_;

  copyState(sta_);
}

void
RepairSetup::repairSetup(float setup_slack_margin,
                         // Percent of violating ends to repair to
                         // reduce tns (0.0-1.0).
                         double repair_tns_end_percent,
                         int max_passes)
{
  init();
  constexpr int digits = 3;
  inserted_buffer_count_ = 0;
  resize_count_ = 0;
  resizer_->buffer_moved_into_core_ = false;

  // Sort failing endpoints by slack.
  VertexSet *endpoints = sta_->endpoints();
  VertexSeq violating_ends;
  for (Vertex *end : *endpoints) {
    Slack end_slack = sta_->vertexSlack(end, max_);
    if (end_slack < setup_slack_margin)
      violating_ends.push_back(end);
  }
  sort(violating_ends, [=](Vertex *end1, Vertex *end2) {
    return sta_->vertexSlack(end1, max_) < sta_->vertexSlack(end2, max_);
  });
  debugPrint(logger_, RSZ, "repair_setup", 1, "Violating endpoints {}/{} {}%",
             violating_ends.size(),
             endpoints->size(),
             int(violating_ends.size() / double(endpoints->size()) * 100));

  int end_index = 0;
  int max_end_count = violating_ends.size() * repair_tns_end_percent;
  // Always repair the worst endpoint, even if tns percent is zero.
  max_end_count = max(max_end_count, 1);
  resizer_->incrementalParasiticsBegin();
  for (Vertex *end : violating_ends) {
    resizer_->updateParasitics();
    sta_->findRequireds();
    Slack end_slack = sta_->vertexSlack(end, max_);
    Slack worst_slack;
    Vertex *worst_vertex;
    sta_->worstSlack(max_, worst_slack, worst_vertex);
    debugPrint(logger_, RSZ, "repair_setup", 1, "{} slack = {} worst_slack = {}",
               end->name(network_),
               delayAsString(end_slack, sta_, digits),
               delayAsString(worst_slack, sta_, digits));
    end_index++;
    if (end_index > max_end_count)
      break;
    Slack prev_end_slack = end_slack;
    Slack prev_worst_slack = worst_slack;
    int pass = 1;
    int decreasing_slack_passes = 0;
    resizer_->journalBegin();
    while (pass <= max_passes) {
      if (end_slack > setup_slack_margin) {
        debugPrint(logger_, RSZ, "repair_setup", 2,
                   "Restoring best slack end slack {} worst slack {}",
                   delayAsString(prev_end_slack, sta_, digits),
                   delayAsString(prev_worst_slack, sta_, digits));
        resizer_->journalRestore(resize_count_, inserted_buffer_count_);
        break;
      }
      PathRef end_path = sta_->vertexWorstSlackPath(end, max_);
      bool changed = repairSetup(end_path, end_slack);
      if (!changed) {
        debugPrint(logger_, RSZ, "repair_setup", 2,
                   "No change after {} decreasing slack passes.",
                   decreasing_slack_passes);
        debugPrint(logger_, RSZ, "repair_setup", 2,
                   "Restoring best slack end slack {} worst slack {}",
                   delayAsString(prev_end_slack, sta_, digits),
                   delayAsString(prev_worst_slack, sta_, digits));
        resizer_->journalRestore(resize_count_, inserted_buffer_count_);
        break;
      }
      resizer_->updateParasitics();
      sta_->findRequireds();
      end_slack = sta_->vertexSlack(end, max_);
      sta_->worstSlack(max_, worst_slack, worst_vertex);
      bool better = (fuzzyGreater(worst_slack, prev_worst_slack)
                     || (end_index != 1
                         && fuzzyEqual(worst_slack, prev_worst_slack)
                         && fuzzyGreater(end_slack, prev_end_slack)));
      debugPrint(logger_, RSZ,
                 "repair_setup", 2, "pass {} slack = {} worst_slack = {} {}",
                 pass,
                 delayAsString(end_slack, sta_, digits),
                 delayAsString(worst_slack, sta_, digits),
                 better ? "save" : "");
      if (better) {
        prev_end_slack = end_slack;
        prev_worst_slack = worst_slack;
        decreasing_slack_passes = 0;
        // Progress, Save checkpoint so we can back up to here.
        resizer_->journalBegin();
      }
      else {
        // Allow slack to increase to get out of local minima.
        // Do not update prev_end_slack so it saves the high water mark.
        decreasing_slack_passes++;
        if (decreasing_slack_passes > decreasing_slack_max_passes_) {
          // Undo changes that reduced slack.
          debugPrint(logger_, RSZ, "repair_setup", 2,
                     "decreasing slack for {} passes.",
                     decreasing_slack_passes);
          debugPrint(logger_, RSZ, "repair_setup", 2,
                     "Restoring best end slack {} worst slack {}",
                     delayAsString(prev_end_slack, sta_, digits),
                     delayAsString(prev_worst_slack, sta_, digits));
          resizer_->journalRestore(resize_count_, inserted_buffer_count_);
          break;
        }
      }
      if (resizer_->overMaxArea())
        break;
      if (end_index == 1)
        end = worst_vertex;
      pass++;
    }
  }
  // Leave the parasitics up to date.
  resizer_->updateParasitics();
  resizer_->incrementalParasiticsEnd();

  if (inserted_buffer_count_ > 0)
    logger_->info(RSZ, 40, "Inserted {} buffers.", inserted_buffer_count_);
  if (resize_count_ > 0)
    logger_->info(RSZ, 41, "Resized {} instances.", resize_count_);
  Slack worst_slack = sta_->worstSlack(max_);
  if (fuzzyLess(worst_slack, setup_slack_margin))
    logger_->warn(RSZ, 62, "Unable to repair all setup violations.");
  if (resizer_->overMaxArea())
    logger_->error(RSZ, 25, "max utilization reached.");
}

// For testing.
void
RepairSetup::repairSetup(Pin *end_pin)
{
  init();
  inserted_buffer_count_ = 0;
  resize_count_ = 0;

  Vertex *vertex = graph_->pinLoadVertex(end_pin);
  Slack slack = sta_->vertexSlack(vertex, max_);
  PathRef path = sta_->vertexWorstSlackPath(vertex, max_);
  resizer_->incrementalParasiticsBegin();
  repairSetup(path, slack);
  // Leave the parasitices up to date.
  resizer_->updateParasitics();
  resizer_->incrementalParasiticsEnd();

  if (inserted_buffer_count_ > 0)
    logger_->info(RSZ, 30, "Inserted {} buffers.", inserted_buffer_count_);
  if (resize_count_ > 0)
    logger_->info(RSZ, 31, "Resized {} instances.", resize_count_);
}

bool
RepairSetup::repairSetup(PathRef &path,
                         Slack path_slack)
{
  PathExpanded expanded(&path, sta_);
  bool changed = false;
  if (expanded.size() > 1) {
    int path_length = expanded.size();
    vector<pair<int, Delay>> load_delays;
    int start_index = expanded.startIndex();
    const DcalcAnalysisPt *dcalc_ap = path.dcalcAnalysisPt(sta_);
    int lib_ap = dcalc_ap->libertyIndex();
    // Find load delay for each gate in the path.
    for (int i = start_index; i < path_length; i++) {
      PathRef *path = expanded.path(i);
      Vertex *path_vertex = path->vertex(sta_);
      const Pin *path_pin = path->pin(sta_);
      if (i > 0
          && network_->isDriver(path_pin)
          && !network_->isTopLevelPort(path_pin)) {
        TimingArc *prev_arc = expanded.prevArc(i);
        TimingArc *corner_arc = prev_arc->cornerArc(lib_ap);
        Edge *prev_edge = path->prevEdge(prev_arc, sta_);
        Delay load_delay = graph_->arcDelay(prev_edge, prev_arc, dcalc_ap->index())
          // Remove intrinsic delay to find load dependent delay.
          - corner_arc->intrinsicDelay();
        load_delays.push_back(pair(i, load_delay));
        debugPrint(logger_, RSZ, "repair_setup", 3, "{} load_delay = {}",
                   path_vertex->name(network_),
                   delayAsString(load_delay, sta_, 3));
      }
    }

    sort(load_delays.begin(), load_delays.end(),
         [](pair<int, Delay> pair1,
            pair<int, Delay> pair2) {
           return pair1.second > pair2.second
             || (pair1.second == pair2.second
                 && pair1.first > pair2.first);
         });
    // Attack gates with largest load delays first.
    for (const auto& [drvr_index, ignored] : load_delays) {
      PathRef *drvr_path = expanded.path(drvr_index);
      Vertex *drvr_vertex = drvr_path->vertex(sta_);
      const Pin *drvr_pin = drvr_vertex->pin();
      const Net *net = network_->net(drvr_pin);
      LibertyPort *drvr_port = network_->libertyPort(drvr_pin);
      LibertyCell *drvr_cell = drvr_port ? drvr_port->libertyCell() : nullptr;
      int fanout = this->fanout(drvr_vertex);
      debugPrint(logger_, RSZ, "repair_setup", 3, "{} {} fanout = {}",
                 network_->pathName(drvr_pin),
                 drvr_cell ? drvr_cell->name() : "none",
                 fanout);

      if (upsizeDrvr(drvr_path, drvr_index, &expanded)) {
        changed = true;
        break;
      }

      // For tristate nets all we can do is resize the driver.
      bool tristate_drvr = resizer_->isTristateDriver(drvr_pin);
      if (fanout > 1
          // Rebuffer blows up on large fanout nets.
          && fanout < rebuffer_max_fanout_
          && !tristate_drvr
          && !resizer_->dontTouch(net)) {
        int rebuffer_count = rebuffer(drvr_pin);
        if (rebuffer_count > 0) {
          debugPrint(logger_, RSZ, "repair_setup", 3, "rebuffer {} inserted {}",
                     network_->pathName(drvr_pin),
                     rebuffer_count);
          inserted_buffer_count_ += rebuffer_count;
          changed = true;
          break;
        }
      }

      // Don't split loads on low fanout nets.
      if (fanout > split_load_min_fanout_
          && !tristate_drvr
          && !resizer_->dontTouch(net)) {
        splitLoads(drvr_path, drvr_index, path_slack, &expanded);
        changed = true;
        break;
      }
    }
  }
  return changed;
}

bool
RepairSetup::upsizeDrvr(PathRef *drvr_path,
                        int drvr_index,
                        PathExpanded *expanded)
{
  Pin *drvr_pin = drvr_path->pin(this);
  Instance *drvr = network_->instance(drvr_pin);
  const DcalcAnalysisPt *dcalc_ap = drvr_path->dcalcAnalysisPt(sta_);
  float load_cap = graph_delay_calc_->loadCap(drvr_pin, dcalc_ap);
  int in_index = drvr_index - 1;
  PathRef *in_path = expanded->path(in_index);
  Pin *in_pin = in_path->pin(sta_);
  LibertyPort *in_port = network_->libertyPort(in_pin);
  if (!resizer_->dontTouch(drvr)) {
    float prev_drive;
    if (drvr_index >= 2) {
      int prev_drvr_index = drvr_index - 2;
      PathRef *prev_drvr_path = expanded->path(prev_drvr_index);
      Pin *prev_drvr_pin = prev_drvr_path->pin(sta_);
      prev_drive = 0.0;
      LibertyPort *prev_drvr_port = network_->libertyPort(prev_drvr_pin);
      if (prev_drvr_port) {
        prev_drive = prev_drvr_port->driveResistance();
      }
    }
    else
      prev_drive = 0.0;
    LibertyPort *drvr_port = network_->libertyPort(drvr_pin);
    LibertyCell *upsize = upsizeCell(in_port, drvr_port, load_cap,
                                     prev_drive, dcalc_ap);
    if (upsize) {
      debugPrint(logger_, RSZ, "repair_setup", 3, "resize {} {} -> {}",
                 network_->pathName(drvr_pin),
                 drvr_port->libertyCell()->name(),
                 upsize->name());
      if (!resizer_->dontTouch(drvr)
          && resizer_->replaceCell(drvr, upsize, true)) {
        resize_count_++;
        return true;
      }
    }
  }
  return false;
}

LibertyCell *
RepairSetup::upsizeCell(LibertyPort *in_port,
                        LibertyPort *drvr_port,
                        float load_cap,
                        float prev_drive,
                        const DcalcAnalysisPt *dcalc_ap)
{
  int lib_ap = dcalc_ap->libertyIndex();
  LibertyCell *cell = drvr_port->libertyCell();
  LibertyCellSeq *equiv_cells = sta_->equivCells(cell);
  if (equiv_cells) {
    const char *in_port_name = in_port->name();
    const char *drvr_port_name = drvr_port->name();
    sort(equiv_cells,
         [=] (const LibertyCell *cell1,
              const LibertyCell *cell2) {
           LibertyPort *port1=cell1->findLibertyPort(drvr_port_name)->cornerPort(lib_ap);
           LibertyPort *port2=cell2->findLibertyPort(drvr_port_name)->cornerPort(lib_ap);
           float drive1 = port1->driveResistance();
           float drive2 = port2->driveResistance();
           ArcDelay intrinsic1 = port1->intrinsicDelay(this);
           ArcDelay intrinsic2 = port2->intrinsicDelay(this);
           return drive1 > drive2
             || ((drive1 == drive2
                  && intrinsic1 < intrinsic2)
                 || (intrinsic1 == intrinsic2
                     && port1->capacitance() < port2->capacitance()));
         });
    float drive = drvr_port->cornerPort(lib_ap)->driveResistance();
    float delay = resizer_->gateDelay(drvr_port, load_cap, resizer_->tgt_slew_dcalc_ap_)
      + prev_drive * in_port->cornerPort(lib_ap)->capacitance();
    for (LibertyCell *equiv : *equiv_cells) {
      LibertyCell *equiv_corner = equiv->cornerCell(lib_ap);
      LibertyPort *equiv_drvr = equiv_corner->findLibertyPort(drvr_port_name);
      LibertyPort *equiv_input = equiv_corner->findLibertyPort(in_port_name);
      float equiv_drive = equiv_drvr->driveResistance();
      // Include delay of previous driver into equiv gate.
      float equiv_delay = resizer_->gateDelay(equiv_drvr, load_cap, dcalc_ap)
        + prev_drive * equiv_input->capacitance();
      if (!resizer_->dontUse(equiv)
          && equiv_drive < drive
          && equiv_delay < delay)
        return equiv;
    }
  }
  return nullptr;
}

void
RepairSetup::splitLoads(PathRef *drvr_path,
                        int drvr_index,
                        Slack drvr_slack,
                        PathExpanded *expanded)
{
  Pin *drvr_pin = drvr_path->pin(this);
  PathRef *load_path = expanded->path(drvr_index + 1);
  Vertex *load_vertex = load_path->vertex(sta_);
  Pin *load_pin = load_vertex->pin();
  // Divide and conquer.
  debugPrint(logger_, RSZ, "repair_setup", 3, "split loads {} -> {}",
             network_->pathName(drvr_pin),
             network_->pathName(load_pin));

  Vertex *drvr_vertex = drvr_path->vertex(sta_);
  const RiseFall *rf = drvr_path->transition(sta_);
  // Sort fanouts of the drvr on the critical path by slack margin
  // wrt the critical path slack.
  vector<pair<Vertex*, Slack>> fanout_slacks;
  VertexOutEdgeIterator edge_iter(drvr_vertex, graph_);
  while (edge_iter.hasNext()) {
    Edge *edge = edge_iter.next();
    Vertex *fanout_vertex = edge->to(graph_);
    Slack fanout_slack = sta_->vertexSlack(fanout_vertex, rf, max_);
    Slack slack_margin = fanout_slack - drvr_slack;
    debugPrint(logger_, RSZ, "repair_setup", 4, " fanin {} slack_margin = {}",
               network_->pathName(fanout_vertex->pin()),
               delayAsString(slack_margin, sta_, 3));
    fanout_slacks.push_back(pair<Vertex*, Slack>(fanout_vertex, slack_margin));
  }

  sort(fanout_slacks.begin(), fanout_slacks.end(),
       [=](pair<Vertex*, Slack> pair1,
           pair<Vertex*, Slack> pair2) {
         return (pair1.second > pair2.second
                 || (pair1.second == pair2.second
                     && network_->pathNameLess(pair1.first->pin(),
                                               pair2.first->pin())));
       });

  Net *net = network_->net(drvr_pin);
  string buffer_name = resizer_->makeUniqueInstName("split");
  Instance *parent = db_network_->topInstance();
  LibertyCell *buffer_cell = resizer_->buffer_lowest_drive_;
  Point drvr_loc = db_network_->location(drvr_pin);
  Instance *buffer = resizer_->makeBuffer(buffer_cell,
                                          buffer_name.c_str(),
                                          parent, drvr_loc);
  inserted_buffer_count_++;

  Net *out_net = resizer_->makeUniqueNet();
  LibertyPort *input, *output;
  buffer_cell->bufferPorts(input, output);

  // Split the loads with extra slack to an inserted buffer.
  // before
  // drvr_pin -> net -> load_pins
  // after
  // drvr_pin -> net -> load_pins with low slack
  //                 -> buffer_in -> net -> rest of loads
  sta_->connectPin(buffer, input, net);
  resizer_->parasiticsInvalid(net);
  sta_->connectPin(buffer, output, out_net);
  int split_index = fanout_slacks.size() / 2;
  for (int i = 0; i < split_index; i++) {
    pair<Vertex*, Slack> fanout_slack = fanout_slacks[i];
    Vertex *load_vertex = fanout_slack.first;
    Pin *load_pin = load_vertex->pin();
    // Leave ports connected to original net so verilog port names are preserved.
    if (!network_->isTopLevelPort(load_pin)) {
      LibertyPort *load_port = network_->libertyPort(load_pin);
      Instance *load = network_->instance(load_pin);

      sta_->disconnectPin(load_pin);
      sta_->connectPin(load, load_port, out_net);
    }
  }
  Pin *buffer_out_pin = network_->findPin(buffer, output);
  resizer_->resizeToTargetSlew(buffer_out_pin);
  resizer_->parasiticsInvalid(net);
  resizer_->parasiticsInvalid(out_net);
}

int
RepairSetup::fanout(Vertex *vertex)
{
  int fanout = 0;
  VertexOutEdgeIterator edge_iter(vertex, graph_);
  while (edge_iter.hasNext()) {
    edge_iter.next();
    fanout++;
  }
  return fanout;
}

} // namespace
