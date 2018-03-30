/* -*- c++ -*- ----------------------------------------------------------
   LAMMPS - Large-scale Atomic/Molecular Massively Parallel Simulator
   http://lammps.sandia.gov, Sandia National Laboratories
   Steve Plimpton, sjplimp@sandia.gov

   Copyright (2003) Sandia Corporation.  Under the terms of Contract
   DE-AC04-94AL85000 with Sandia Corporation, the U.S. Government retains
   certain rights in this software.  This software is distributed under
   the GNU General Public License.

   See the README file in the top-level LAMMPS directory.
------------------------------------------------------------------------- */

#ifdef FIX_CLASS

FixStyle(neb_mod,FixNEB_MOD)

#else

#ifndef LMP_FIX_NEB_MOD_H
#define LMP_FIX_NEB_MOD_H

#include "fix.h"

namespace LAMMPS_NS {

class FixNEB_MOD : public Fix {
 public:
  double veng,plen,nlen,dotpath,dottangrad,gradlen,dotgrad;
  double kspring, freplicasum;
  double kprev, knext;
  int rclimber;

  FixNEB_MOD(class LAMMPS *, int, char **);
  ~FixNEB_MOD();
  int setmask();
  void init();
  void min_setup(int);
  void min_post_force(int);

 private:
  int me,nprocs,nprocs_universe;
  double kspringIni,kspringFinal,kspringPerp,EIniIni,EFinalIni;
  bool StandardNEB_MOD,NEB_MODLongRange,PerpSpring,FreeEndIni,FreeEndFinal;
  bool FreeEndFinalWithRespToEIni,FinalAndInterWithRespToEIni;
  bool AutoKspring;  // for automatic update of spring constant
  bool VariableKspring;

  int ireplica,nreplica;
  int procnext,procprev;
  int cmode;
  MPI_Comm uworld;
  MPI_Comm rootworld;


  char *id_pe;
  class Compute *pe;

  int neb_modatoms;
  int ntotal;                  // total # of atoms, NEB_MOD or not
  int maxlocal;                // size of xprev,xnext,tangent arrays
  double *nlenall;
  double *freplicaall;
  double *gradlenall;
  double **xprev,**xnext,**fnext,**springF;
  double **tangent;
  double **xsend,**xrecv;      // coords to send/recv to/from other replica
  double **fsend,**frecv;      // coords to send/recv to/from other replica
  tagint *tagsend,*tagrecv;    // ditto for atom IDs

                                 // info gathered from all procs in my replica
  double **xsendall,**xrecvall;    // coords to send/recv to/from other replica
  double **fsendall,**frecvall;    // force to send/recv to/from other replica
  tagint *tagsendall,*tagrecvall;  // ditto for atom IDs

  int *counts,*displacements;   // used for MPI_Gather
  double kmax, kdelta; // used for variable spring constant
  

  void inter_replica_comm();
  void reallocate();

  double gradl;
};

}

#endif
#endif

/* ERROR/WARNING messages:

E: Illegal ... command

Self-explanatory.  Check the input script syntax and compare to the
documentation for the command.  You can use -echo screen as a
command-line option when running LAMMPS to see the offending line.

E: Potential energy ID for fix neb_mod does not exist

Self-explanatory.

E: Atom count changed in fix neb_mod

This is not allowed in a NEB calculation.

*/
