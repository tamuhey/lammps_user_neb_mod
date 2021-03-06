/* ----------------------------------------------------------------------
   LAMMPS - Large-scale Atomic/Molecular Massively Parallel Simulator
   http://lammps.sandia.gov, Sandia National Laboratories
   Steve Plimpton, sjplimp@sandia.gov

   Copyright (2003) Sandia Corporation.  Under the terms of Contract
   DE-AC04-94AL85000 with Sandia Corporation, the U.S. Government retains
   certain rights in this software.  This software is distributed under
   the GNU General Public License.

   See the README file in the top-level LAMMPS directory.
------------------------------------------------------------------------- */

/* ----------------------------------------------------------------------
   Contributing author for: Emile Maras (CEA, France)
     new options for inter-replica forces, first/last replica treatment
------------------------------------------------------------------------- */

#include <mpi.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "fix_neb_mod.h"
#include "universe.h"
#include "update.h"
#include "atom.h"
#include "domain.h"
#include "comm.h"
#include "modify.h"
#include "compute.h"
#include "group.h"
#include "memory.h"
#include "error.h"
#include "force.h"
#include "math_const.h"

using namespace LAMMPS_NS;
using namespace FixConst;
using namespace MathConst;

enum{SINGLE_PROC_DIRECT,SINGLE_PROC_MAP,MULTI_PROC};

#define BUFSIZE 8

/* ---------------------------------------------------------------------- */

FixNEB_MOD::FixNEB_MOD(LAMMPS *lmp, int narg, char **arg) :
  Fix(lmp, narg, arg),
  id_pe(NULL), pe(NULL), nlenall(NULL), xprev(NULL), xnext(NULL),
  fnext(NULL), springF(NULL), tangent(NULL), xsend(NULL), xrecv(NULL),
  fsend(NULL), frecv(NULL), tagsend(NULL), tagrecv(NULL),
  xsendall(NULL), xrecvall(NULL), fsendall(NULL), frecvall(NULL),
  tagsendall(NULL), tagrecvall(NULL), counts(NULL),
  displacements(NULL),freplicaall(NULL)
{

  if (narg < 4) error->all(FLERR,"Illegal fix neb_mod command");

  kspring = force->numeric(FLERR,arg[3]);
  if (kspring <= 0.0) error->all(FLERR,"Illegal fix neb_mod command");

  // optional params

  NEB_MODLongRange = false;
  StandardNEB_MOD = true;
  AutomaticUpdateKspring = false;
  PerpSpring = FreeEndIni = FreeEndFinal = false;
  FreeEndFinalWithRespToEIni = FinalAndInterWithRespToEIni = false;
  kspringPerp = 0.0;
  kspringIni = 1.0;
  kspringFinal = 1.0;

  int iarg = 4;
  while (iarg < narg) {
    if (strcmp(arg[iarg],"parallel") == 0) {
      if (iarg+2 > narg) error->all(FLERR,"Illegal fix neb_mod command");
      if (strcmp(arg[iarg+1],"ideal") == 0) {
        NEB_MODLongRange = true;
        StandardNEB_MOD = false;
      } else if (strcmp(arg[iarg+1],"neigh") == 0) {
        NEB_MODLongRange = false;
        StandardNEB_MOD = true;
      } else error->all(FLERR,"Illegal fix neb_mod command");
      iarg += 2;
    } else if (strcmp(arg[iarg], "auto") == 0) {
      AutomaticUpdateKspring = true;
      iarg += 1;

    } else if (strcmp(arg[iarg],"perp") == 0) {
      if (iarg+2 > narg) error->all(FLERR,"Illegal fix neb_mod command");
      PerpSpring = true;
      kspringPerp = force->numeric(FLERR,arg[iarg+1]);
      if (kspringPerp == 0.0) PerpSpring = false;
      if (kspringPerp < 0.0) error->all(FLERR,"Illegal fix neb_mod command");
      iarg += 2;

    } else if (strcmp (arg[iarg],"end") == 0) {
      if (iarg+3 > narg) error->all(FLERR,"Illegal fix neb_mod command");
      if (strcmp(arg[iarg+1],"first") == 0) {
        FreeEndIni = true;
	kspringIni = force->numeric(FLERR,arg[iarg+2]);
      } else if (strcmp(arg[iarg+1],"last") == 0) {
        FreeEndFinal = true;
        FinalAndInterWithRespToEIni = false;
        FreeEndFinalWithRespToEIni = false;
	kspringFinal = force->numeric(FLERR,arg[iarg+2]);
      } else if (strcmp(arg[iarg+1],"last/efirst") == 0) {
        FreeEndFinal = false;
        FinalAndInterWithRespToEIni = false;
        FreeEndFinalWithRespToEIni = true;
	kspringFinal = force->numeric(FLERR,arg[iarg+2]);
      } else if (strcmp(arg[iarg+1],"last/efirst/middle") == 0) {
        FreeEndFinal = false;
        FinalAndInterWithRespToEIni = true;
        FreeEndFinalWithRespToEIni = true;
	kspringFinal = force->numeric(FLERR,arg[iarg+2]);
      } else error->all(FLERR,"Illegal fix neb_mod command");

      iarg += 3;
    
    } else error->all(FLERR,"Illegal fix neb_mod command");
  }

  // nreplica = number of partitions
  // ireplica = which world I am in universe
  // nprocs_universe = # of procs in all replicase
  // procprev,procnext = root proc in adjacent replicas

  me = comm->me;
  nprocs = comm->nprocs;

  nprocs_universe = universe->nprocs;
  nreplica = universe->nworlds;
  ireplica = universe->iworld;

  if (ireplica > 0) procprev = universe->root_proc[ireplica-1];
  else procprev = -1;
  if (ireplica < nreplica-1) procnext = universe->root_proc[ireplica+1];
  else procnext = -1;

  uworld = universe->uworld;
  int *iroots = new int[nreplica];
  MPI_Group uworldgroup,rootgroup;
  if (NEB_MODLongRange || AutomaticUpdateKspring) {
    for (int i=0; i<nreplica; i++)
      iroots[i] = universe->root_proc[i];
    MPI_Comm_group(uworld, &uworldgroup);
    MPI_Group_incl(uworldgroup, nreplica, iroots, &rootgroup);
    MPI_Comm_create(uworld, rootgroup, &rootworld);
  }
  delete [] iroots;

  // create a new compute pe style
  // id = fix-ID + pe, compute group = all

  int n = strlen(id) + 4;
  id_pe = new char[n];
  strcpy(id_pe,id);
  strcat(id_pe,"_pe");

  char **newarg = new char*[3];
  newarg[0] = id_pe;
  newarg[1] = (char *) "all";
  newarg[2] = (char *) "pe";
  modify->add_compute(3,newarg);
  delete [] newarg;

  // initialize local storage

  maxlocal = -1;
  ntotal = -1;
}

/* ---------------------------------------------------------------------- */

FixNEB_MOD::~FixNEB_MOD()
{
  modify->delete_compute(id_pe);
  delete [] id_pe;

  memory->destroy(xprev);
  memory->destroy(xnext);
  memory->destroy(tangent);
  memory->destroy(fnext);
  memory->destroy(springF);
  memory->destroy(xsend);
  memory->destroy(xrecv);
  memory->destroy(fsend);
  memory->destroy(frecv);
  memory->destroy(tagsend);
  memory->destroy(tagrecv);

  memory->destroy(xsendall);
  memory->destroy(xrecvall);
  memory->destroy(fsendall);
  memory->destroy(frecvall);
  memory->destroy(tagsendall);
  memory->destroy(tagrecvall);

  memory->destroy(counts);
  memory->destroy(displacements);

  if (NEB_MODLongRange || AutomaticUpdateKspring) {
    if (rootworld != MPI_COMM_NULL) MPI_Comm_free(&rootworld);
    memory->destroy(nlenall);
  }
  if (AutomaticUpdateKspring) {
    if (rootworld != MPI_COMM_NULL) MPI_Comm_free(&rootworld);
    memory->destroy(freplicaall);
  }
}

/* ---------------------------------------------------------------------- */

int FixNEB_MOD::setmask()
{
  int mask = 0;
  mask |= MIN_POST_FORCE;
  return mask;
}

/* ---------------------------------------------------------------------- */

void FixNEB_MOD::init()
{
  int icompute = modify->find_compute(id_pe);
  if (icompute < 0)
    error->all(FLERR,"Potential energy ID for fix neb_mod does not exist");
  pe = modify->compute[icompute];

  // turn off climbing mode, NEB_MOD command turns it on after init()

  rclimber = -1;

  // neb_modatoms = # of atoms in fix group = atoms with inter-replica forces

  bigint count = group->count(igroup);
  if (count > MAXSMALLINT) error->all(FLERR,"Too many active NEB_MOD atoms");
  neb_modatoms = count;

  // comm mode for inter-replica exchange of coords

  if (nreplica == nprocs_universe &&
      neb_modatoms == atom->natoms && atom->sortfreq == 0)
    cmode = SINGLE_PROC_DIRECT;
  else if (nreplica == nprocs_universe) cmode = SINGLE_PROC_MAP;
  else cmode = MULTI_PROC;

  // ntotal = total # of atoms in system, NEB_MOD atoms or not

  if (atom->natoms > MAXSMALLINT) error->all(FLERR,"Too many atoms for NEB_MOD");
  ntotal = atom->natoms;

  if (atom->nmax > maxlocal) reallocate();

  if (MULTI_PROC && counts == NULL) {
    memory->create(xsendall,ntotal,3,"neb_mod:xsendall");
    memory->create(xrecvall,ntotal,3,"neb_mod:xrecvall");
    memory->create(fsendall,ntotal,3,"neb_mod:fsendall");
    memory->create(frecvall,ntotal,3,"neb_mod:frecvall");
    memory->create(tagsendall,ntotal,"neb_mod:tagsendall");
    memory->create(tagrecvall,ntotal,"neb_mod:tagrecvall");
    memory->create(counts,nprocs,"neb_mod:counts");
    memory->create(displacements,nprocs,"neb_mod:displacements");
  }
}

/* ---------------------------------------------------------------------- */

void FixNEB_MOD::min_setup(int vflag)
{
  min_post_force(vflag);

  // trigger potential energy computation on next timestep

  pe->addstep(update->ntimestep+1);
}

/* ---------------------------------------------------------------------- */

void FixNEB_MOD::min_post_force(int vflag)
{
  double vprev,vnext;
  double delxp,delyp,delzp,delxn,delyn,delzn;
  double vIni=0.0;

  vprev = vnext = veng = pe->compute_scalar();

  if (ireplica < nreplica-1 && me == 0)
    MPI_Send(&veng,1,MPI_DOUBLE,procnext,0,uworld);
  if (ireplica > 0 && me == 0)
    MPI_Recv(&vprev,1,MPI_DOUBLE,procprev,0,uworld,MPI_STATUS_IGNORE);

  if (ireplica > 0 && me == 0)
    MPI_Send(&veng,1,MPI_DOUBLE,procprev,0,uworld);
  if (ireplica < nreplica-1 && me == 0)
    MPI_Recv(&vnext,1,MPI_DOUBLE,procnext,0,uworld,MPI_STATUS_IGNORE);

  if (cmode == MULTI_PROC) {
    MPI_Bcast(&vprev,1,MPI_DOUBLE,0,world);
    MPI_Bcast(&vnext,1,MPI_DOUBLE,0,world);
  }

  if (FreeEndFinal && ireplica == nreplica-1 && (update->ntimestep == 0)) EFinalIni = veng;

  if (ireplica == 0) vIni=veng;

  if (FreeEndFinalWithRespToEIni) {
    if (me == 0) {
      int procFirst;
      procFirst=universe->root_proc[0];
      MPI_Bcast(&vIni,1,MPI_DOUBLE,procFirst,uworld);
    }
    if (cmode == MULTI_PROC) {
      MPI_Bcast(&vIni,1,MPI_DOUBLE,0,world);
    }
  }

  if (FreeEndIni && ireplica == 0 && (update->ntimestep == 0)) EIniIni = veng;
  /*  if (FreeEndIni && ireplica == 0) {
    //    if (me == 0 )
      if (update->ntimestep == 0) {
        EIniIni = veng;
	//	if (cmode == MULTI_PROC)
	// MPI_Bcast(&EIniIni,1,MPI_DOUBLE,0,world);
      }
      }*/

  // communicate atoms to/from adjacent replicas to fill xprev,xnext

  inter_replica_comm();

  // trigger potential energy computation on next timestep

  pe->addstep(update->ntimestep+1);

  double **x = atom->x;
  int *mask = atom->mask;
  double dot = 0.0;
  double prefactor = 0.0;

  double **f = atom->f;
  int nlocal = atom->nlocal;

  //calculating separation between images

  plen = 0.0;
  nlen = 0.0;
  double tlen = 0.0;
  double gradnextlen = 0.0;

  dotgrad = gradlen = dotpath = dottangrad = 0.0;

  if (ireplica == nreplica-1) {

    for (int i = 0; i < nlocal; i++)
      if (mask[i] & groupbit) {
        delxp = x[i][0] - xprev[i][0];
        delyp = x[i][1] - xprev[i][1];
        delzp = x[i][2] - xprev[i][2];
        domain->minimum_image(delxp,delyp,delzp);
        plen += delxp*delxp + delyp*delyp + delzp*delzp;
        dottangrad += delxp* f[i][0]+ delyp*f[i][1]+delzp*f[i][2];
        gradlen += f[i][0]*f[i][0] + f[i][1]*f[i][1] + f[i][2]*f[i][2];
        if (FreeEndFinal||FreeEndFinalWithRespToEIni) {
          tangent[i][0]=delxp;
          tangent[i][1]=delyp;
          tangent[i][2]=delzp;
          tlen += tangent[i][0]*tangent[i][0] +
            tangent[i][1]*tangent[i][1] + tangent[i][2]*tangent[i][2];
          dot += f[i][0]*tangent[i][0] + f[i][1]*tangent[i][1] + 
            f[i][2]*tangent[i][2];
        }
      }

  } else if (ireplica == 0) {
    for (int i = 0; i < nlocal; i++)
      if (mask[i] & groupbit) {
        delxn = xnext[i][0] - x[i][0];
        delyn = xnext[i][1] - x[i][1];
        delzn = xnext[i][2] - x[i][2];
        domain->minimum_image(delxn,delyn,delzn);
        nlen += delxn*delxn + delyn*delyn + delzn*delzn;
        gradnextlen += fnext[i][0]*fnext[i][0]
          + fnext[i][1]*fnext[i][1] +fnext[i][2] * fnext[i][2];
        dotgrad += f[i][0]*fnext[i][0]
          + f[i][1]*fnext[i][1] + f[i][2]*fnext[i][2];
        dottangrad += delxn*f[i][0]+ delyn*f[i][1] + delzn*f[i][2];
        gradlen += f[i][0]*f[i][0] + f[i][1]*f[i][1] + f[i][2]*f[i][2];
        if (FreeEndIni) {
          tangent[i][0]=delxn;
          tangent[i][1]=delyn;
          tangent[i][2]=delzn;
          tlen += tangent[i][0]*tangent[i][0] + 
            tangent[i][1]*tangent[i][1] + tangent[i][2]*tangent[i][2];
          dot += f[i][0]*tangent[i][0] + f[i][1]*tangent[i][1] + 
            f[i][2]*tangent[i][2];
        }
      }
  } else {

    // not the first or last replica

    double vmax = MAX(fabs(vnext-veng),fabs(vprev-veng));
    double vmin = MIN(fabs(vnext-veng),fabs(vprev-veng));


    for (int i = 0; i < nlocal; i++)
      if (mask[i] & groupbit) {
        delxp = x[i][0] - xprev[i][0];
        delyp = x[i][1] - xprev[i][1];
        delzp = x[i][2] - xprev[i][2];
        domain->minimum_image(delxp,delyp,delzp);
        plen += delxp*delxp + delyp*delyp + delzp*delzp;

        delxn = xnext[i][0] - x[i][0];
        delyn = xnext[i][1] - x[i][1];
        delzn = xnext[i][2] - x[i][2];
        domain->minimum_image(delxn,delyn,delzn);

        if (vnext > veng && veng > vprev) {
          tangent[i][0] = delxn;
          tangent[i][1] = delyn;
          tangent[i][2] = delzn;
        } else if (vnext < veng && veng < vprev) {
          tangent[i][0] = delxp;
          tangent[i][1] = delyp;
          tangent[i][2] = delzp;
        } else {
          if (vnext > vprev) {
            tangent[i][0] = vmax*delxn + vmin*delxp;
            tangent[i][1] = vmax*delyn + vmin*delyp;
            tangent[i][2] = vmax*delzn + vmin*delzp;
          } else {
            tangent[i][0] = vmin*delxn + vmax*delxp;
            tangent[i][1] = vmin*delyn + vmax*delyp;
            tangent[i][2] = vmin*delzn + vmax*delzp;
          }
        }

        nlen += delxn*delxn + delyn*delyn + delzn*delzn;
        tlen += tangent[i][0]*tangent[i][0] + 
          tangent[i][1]*tangent[i][1] + tangent[i][2]*tangent[i][2];
        gradlen += f[i][0]*f[i][0] + f[i][1]*f[i][1] + f[i][2]*f[i][2];
        dotpath += delxp*delxn + delyp*delyn + delzp*delzn;
        dottangrad += tangent[i][0]*f[i][0] + 
          tangent[i][1]*f[i][1] + tangent[i][2]*f[i][2];
        gradnextlen += fnext[i][0]*fnext[i][0] + 
          fnext[i][1]*fnext[i][1] +fnext[i][2] * fnext[i][2];
        dotgrad += f[i][0]*fnext[i][0] + f[i][1]*fnext[i][1] + 
          f[i][2]*fnext[i][2];

        springF[i][0] = kspringPerp*(delxn-delxp);
        springF[i][1] = kspringPerp*(delyn-delyp);
        springF[i][2] = kspringPerp*(delzn-delzp);
      }
  }

  double bufin[BUFSIZE], bufout[BUFSIZE];
  bufin[0] = nlen;
  bufin[1] = plen;
  bufin[2] = tlen;
  bufin[3] = gradlen;
  bufin[4] = gradnextlen;
  bufin[5] = dotpath;
  bufin[6] = dottangrad;
  bufin[7] = dotgrad;
  MPI_Allreduce(bufin,bufout,BUFSIZE,MPI_DOUBLE,MPI_SUM,world);
  nlen = sqrt(bufout[0]);
  plen = sqrt(bufout[1]);
  tlen = sqrt(bufout[2]);
  gradlen = sqrt(bufout[3]);
  gradnextlen = sqrt(bufout[4]);
  dotpath = bufout[5];
  dottangrad = bufout[6];
  dotgrad = bufout[7];

  // normalize tangent vector

  if (tlen > 0.0) {
    double tleninv = 1.0/tlen;
    for (int i = 0; i < nlocal; i++)
      if (mask[i] & groupbit) {
        tangent[i][0] *= tleninv;
        tangent[i][1] *= tleninv;
        tangent[i][2] *= tleninv;
      }
  }

  // first or last replica has no change to forces, just return
  // *not return for Automatic Update Kspring 

  if (ireplica > 0 && ireplica < nreplica-1)
    dottangrad = dottangrad/(tlen*gradlen);
  if (ireplica == 0)
    dottangrad = dottangrad/(nlen*gradlen);
  if (ireplica == nreplica-1)
    dottangrad = dottangrad/(plen*gradlen);
  if (ireplica < nreplica-1)
    dotgrad = dotgrad /(gradlen*gradnextlen);

  if (FreeEndIni && ireplica == 0) {
    if (tlen > 0.0) {
      double dotall;
      MPI_Allreduce(&dot,&dotall,1,MPI_DOUBLE,MPI_SUM,world);
      dot=dotall/tlen;

      if (dot<0) prefactor = -dot - kspringIni*(veng-EIniIni);
      else prefactor = -dot + kspringIni*(veng-EIniIni);

      for (int i = 0; i < nlocal; i++)
        if (mask[i] & groupbit) {
          f[i][0] += prefactor *tangent[i][0];
          f[i][1] += prefactor *tangent[i][1];
          f[i][2] += prefactor *tangent[i][2];
        }
    }
  }

  if (FreeEndFinal && ireplica == nreplica -1) {
    if (tlen > 0.0) {
      double dotall;
      MPI_Allreduce(&dot,&dotall,1,MPI_DOUBLE,MPI_SUM,world);
      dot=dotall/tlen;

      if (dot<0) prefactor = -dot - kspringFinal*(veng-EFinalIni);
      else prefactor = -dot + kspringFinal*(veng-EFinalIni);

      for (int i = 0; i < nlocal; i++)
        if (mask[i] & groupbit) {
          f[i][0] += prefactor *tangent[i][0];
          f[i][1] += prefactor *tangent[i][1];
          f[i][2] += prefactor *tangent[i][2];
        }
    }
  }

  if (FreeEndFinalWithRespToEIni&&ireplica == nreplica -1) {
    if (tlen > 0.0) {
      double dotall;
      MPI_Allreduce(&dot,&dotall,1,MPI_DOUBLE,MPI_SUM,world);
      dot=dotall/tlen;

      if (dot<0) prefactor = -dot - kspringFinal*(veng-vIni);
      else prefactor = -dot + kspringFinal*(veng-vIni);

      for (int i = 0; i < nlocal; i++)
        if (mask[i] & groupbit) {
          f[i][0] += prefactor *tangent[i][0];
          f[i][1] += prefactor *tangent[i][1];
          f[i][2] += prefactor *tangent[i][2];
        }
    }
  }

  double lentot = 0;
  double meanDist,idealPos,lenuntilIm,lenuntilClimber;
  lenuntilClimber=0;
  if (NEB_MODLongRange || AutomaticUpdateKspring) {
    if (cmode == SINGLE_PROC_DIRECT || cmode == SINGLE_PROC_MAP) {
      MPI_Allgather(&nlen,1,MPI_DOUBLE,&nlenall[0],1,MPI_DOUBLE,uworld);
    } else {
      if (me == 0)
        MPI_Allgather(&nlen,1,MPI_DOUBLE,&nlenall[0],1,MPI_DOUBLE,rootworld);
      MPI_Bcast(nlenall,nreplica,MPI_DOUBLE,0,world);
    }
    for (int i = 0; i < nreplica; i++)
      lentot += nlenall[i];
  }

  if (NEB_MODLongRange){
    lenuntilIm = 0;
    for (int i = 0; i < ireplica; i++)
      lenuntilIm += nlenall[i];
    if (NEB_MODLongRange) {
      meanDist = lentot/(nreplica -1);

      if (rclimber>0) {
        for (int i = 0; i < rclimber; i++)
          lenuntilClimber += nlenall[i];
        double meanDistBeforeClimber = lenuntilClimber/rclimber;
        double meanDistAfterClimber =
          (lentot-lenuntilClimber)/(nreplica-rclimber-1);
        if (ireplica<rclimber)
          idealPos = ireplica * meanDistBeforeClimber;
        else
          idealPos = lenuntilClimber+ (ireplica-rclimber)*meanDistAfterClimber;
      } else idealPos = ireplica * meanDist;
    }
  }

  //if (ireplica == 0 || ireplica == nreplica-1) return ;
  if (ireplica > 0 && ireplica < nreplica-1) {

  double AngularContr;
  dotpath = dotpath/(plen*nlen);
  AngularContr = 0.5 *(1+cos(MY_PI * dotpath));

  double dotSpringTangent;
  dotSpringTangent=0;

  for (int i = 0; i < nlocal; i++) {
    if (mask[i] & groupbit) {
      dot += f[i][0]*tangent[i][0] + f[i][1]*tangent[i][1] + 
        f[i][2]*tangent[i][2];
      dotSpringTangent += springF[i][0]*tangent[i][0] +
        springF[i][1]*tangent[i][1] + springF[i][2]*tangent[i][2];}
  }

  double dotSpringTangentall;
  MPI_Allreduce(&dotSpringTangent,&dotSpringTangentall,1,
                MPI_DOUBLE,MPI_SUM,world);
  dotSpringTangent=dotSpringTangentall;
  double dotall;
  MPI_Allreduce(&dot,&dotall,1,MPI_DOUBLE,MPI_SUM,world);
  dot=dotall;

  if (ireplica == rclimber) prefactor = -2.0*dot;
  else {
    if (NEB_MODLongRange) {
      prefactor = -dot - kspring*(lenuntilIm-idealPos)/(2*meanDist);
    } else if (StandardNEB_MOD) {
      prefactor = -dot + kspring*(nlen-plen);
    }

    if (FinalAndInterWithRespToEIni&& veng<vIni) {
      for (int i = 0; i < nlocal; i++)
        if (mask[i] & groupbit) {
          f[i][0] = 0;
          f[i][1] = 0;
          f[i][2] = 0;
        }

      if (NEB_MODLongRange) {
        prefactor =  kspring*(lenuntilIm-idealPos)/(2*meanDist);
      } else if (StandardNEB_MOD) {
        prefactor = kspring*(nlen-plen);
      }
      AngularContr=0;
    }
  }

  freplica_norm2 = 0.0;
  double *freplica = new double[3]; // fnudge and fperp
  for (int i = 0; i < nlocal; i++)
    if (mask[i] & groupbit) {
      freplica[0] = prefactor*tangent[i][0] + 
        AngularContr*(springF[i][0] - dotSpringTangent*tangent[i][0]);
      freplica[1] = prefactor*tangent[i][1] + 
        AngularContr*(springF[i][1] - dotSpringTangent*tangent[i][1]);
      freplica[2] = prefactor*tangent[i][2] + 
        AngularContr*(springF[i][2] - dotSpringTangent*tangent[i][2]);

      f[i][0] += freplica[0];
      f[i][1] += freplica[1];
      f[i][2] += freplica[2];

      freplica_norm2 += sqrt(freplica[0]*freplica[0] + freplica[1] * freplica[1] + freplica[2]* freplica[2]);
    }
  delete [] freplica;
  }
  else{
  freplica_norm2 = 0.0;}

  if (AutomaticUpdateKspring) {
    double freplica_norm2all;
    MPI_Allreduce(&freplica_norm2,&freplica_norm2all,1,MPI_DOUBLE,MPI_SUM,world);
    freplica_norm2 = freplica_norm2all;

    if (cmode == SINGLE_PROC_DIRECT || cmode == SINGLE_PROC_MAP) {
      MPI_Allgather(&freplica_norm2,1,MPI_DOUBLE,&freplicaall[0],1,MPI_DOUBLE,uworld);
    } else {
      if (me == 0)
        MPI_Allgather(&freplica_norm2,1,MPI_DOUBLE,&freplicaall[0],1,MPI_DOUBLE,rootworld);
      MPI_Bcast(freplicaall,nreplica,MPI_DOUBLE,0,world);
    }
    freplicasum = 0;
    for (int i = 0; i < nreplica; i++)
      freplicasum += freplicaall[i];

    kspring = freplicasum /lentot;
  }
}

/* ----------------------------------------------------------------------
   send/recv NEB_MOD atoms to/from adjacent replicas
   received atoms matching my local atoms are stored in xprev,xnext
   replicas 0 and N-1 send but do not receive any atoms
------------------------------------------------------------------------- */


void FixNEB_MOD::inter_replica_comm()
{
  int i,m;
  MPI_Request request;
  MPI_Request requests[2];
  MPI_Status statuses[2];

  // reallocate memory if necessary

  if (atom->nmax > maxlocal) reallocate();

  double **x = atom->x;
  double **f = atom->f;
  tagint *tag = atom->tag;
  int *mask = atom->mask;
  int nlocal = atom->nlocal;

  // -----------------------------------------------------
  // 3 cases: two for single proc per replica
  //          one for multiple procs per replica
  // -----------------------------------------------------

  // single proc per replica
  // all atoms are NEB_MOD atoms and no atom sorting
  // direct comm of x -> xprev and x -> xnext

  if (cmode == SINGLE_PROC_DIRECT) {
    if (ireplica > 0)
      MPI_Irecv(xprev[0],3*nlocal,MPI_DOUBLE,procprev,0,uworld,&request);
    if (ireplica < nreplica-1)
      MPI_Send(x[0],3*nlocal,MPI_DOUBLE,procnext,0,uworld);
    if (ireplica > 0) MPI_Wait(&request,MPI_STATUS_IGNORE);
    if (ireplica < nreplica-1)
      MPI_Irecv(xnext[0],3*nlocal,MPI_DOUBLE,procnext,0,uworld,&request);
    if (ireplica > 0)
      MPI_Send(x[0],3*nlocal,MPI_DOUBLE,procprev,0,uworld);
    if (ireplica < nreplica-1) MPI_Wait(&request,MPI_STATUS_IGNORE);

    if (ireplica < nreplica-1)
      MPI_Irecv(fnext[0],3*nlocal,MPI_DOUBLE,procnext,0,uworld,&request);
    if (ireplica > 0)
      MPI_Send(f[0],3*nlocal,MPI_DOUBLE,procprev,0,uworld);
    if (ireplica < nreplica-1) MPI_Wait(&request,MPI_STATUS_IGNORE);

    return;
  }

  // single proc per replica
  // but only some atoms are NEB_MOD atoms or atom sorting is enabled
  // send atom IDs and coords of only NEB_MOD atoms to prev/next proc
  // recv procs use atom->map() to match received coords to owned atoms

  if (cmode == SINGLE_PROC_MAP) {
    m = 0;
    for (i = 0; i < nlocal; i++)
      if (mask[i] & groupbit) {
        tagsend[m] = tag[i];
        xsend[m][0] = x[i][0];
        xsend[m][1] = x[i][1];
        xsend[m][2] = x[i][2];
        fsend[m][0] = f[i][0];
        fsend[m][1] = f[i][1];
        fsend[m][2] = f[i][2];
        m++;
      }

    if (ireplica > 0) {
      MPI_Irecv(xrecv[0],3*neb_modatoms,MPI_DOUBLE,procprev,0,uworld,&requests[0]);
      MPI_Irecv(tagrecv,neb_modatoms,MPI_LMP_TAGINT,procprev,0,uworld,&requests[1]);
    }
    if (ireplica < nreplica-1) {
      MPI_Send(xsend[0],3*neb_modatoms,MPI_DOUBLE,procnext,0,uworld);
      MPI_Send(tagsend,neb_modatoms,MPI_LMP_TAGINT,procnext,0,uworld);
    }

    if (ireplica > 0) {
      MPI_Waitall(2,requests,statuses);
      for (i = 0; i < neb_modatoms; i++) {
        m = atom->map(tagrecv[i]);
        xprev[m][0] = xrecv[i][0];
        xprev[m][1] = xrecv[i][1];
        xprev[m][2] = xrecv[i][2];
      }
    }
    if (ireplica < nreplica-1) {
      MPI_Irecv(xrecv[0],3*neb_modatoms,MPI_DOUBLE,procnext,0,uworld,&requests[0]);
      MPI_Irecv(frecv[0],3*neb_modatoms,MPI_DOUBLE,procnext,0,uworld,&requests[0]);
      MPI_Irecv(tagrecv,neb_modatoms,MPI_LMP_TAGINT,procnext,0,uworld,&requests[1]);
    }
    if (ireplica > 0) {
      MPI_Send(xsend[0],3*neb_modatoms,MPI_DOUBLE,procprev,0,uworld);
      MPI_Send(fsend[0],3*neb_modatoms,MPI_DOUBLE,procprev,0,uworld);
      MPI_Send(tagsend,neb_modatoms,MPI_LMP_TAGINT,procprev,0,uworld);
    }

    if (ireplica < nreplica-1) {
      MPI_Waitall(2,requests,statuses);
      for (i = 0; i < neb_modatoms; i++) {
        m = atom->map(tagrecv[i]);
        xnext[m][0] = xrecv[i][0];
        xnext[m][1] = xrecv[i][1];
        xnext[m][2] = xrecv[i][2];
        fnext[m][0] = frecv[i][0];
        fnext[m][1] = frecv[i][1];
        fnext[m][2] = frecv[i][2];
      }
    }

    return;
  }

  // multiple procs per replica
  // MPI_Gather all coords and atom IDs to root proc of each replica
  // send to root of adjacent replicas
  // bcast within each replica
  // each proc extracts info for atoms it owns via atom->map()

  m = 0;
  for (i = 0; i < nlocal; i++)
    if (mask[i] & groupbit) {
      tagsend[m] = tag[i];
      xsend[m][0] = x[i][0];
      xsend[m][1] = x[i][1];
      xsend[m][2] = x[i][2];
      fsend[m][0] = f[i][0];
      fsend[m][1] = f[i][1];
      fsend[m][2] = f[i][2];
      m++;
    }

  MPI_Gather(&m,1,MPI_INT,counts,1,MPI_INT,0,world);
  displacements[0] = 0;
  for (i = 0; i < nprocs-1; i++)
    displacements[i+1] = displacements[i] + counts[i];
  MPI_Gatherv(tagsend,m,MPI_LMP_TAGINT,
              tagsendall,counts,displacements,MPI_LMP_TAGINT,0,world);
  for (i = 0; i < nprocs; i++) counts[i] *= 3;
  for (i = 0; i < nprocs-1; i++)
    displacements[i+1] = displacements[i] + counts[i];
  if (xsend) {
    MPI_Gatherv(xsend[0],3*m,MPI_DOUBLE,
                xsendall[0],counts,displacements,MPI_DOUBLE,0,world);
    MPI_Gatherv(fsend[0],3*m,MPI_DOUBLE,
                fsendall[0],counts,displacements,MPI_DOUBLE,0,world);
  } else {
    MPI_Gatherv(NULL,3*m,MPI_DOUBLE,
                xsendall[0],counts,displacements,MPI_DOUBLE,0,world);
    MPI_Gatherv(NULL,3*m,MPI_DOUBLE,
                fsendall[0],counts,displacements,MPI_DOUBLE,0,world);
  }

  if (ireplica > 0 && me == 0) {
    MPI_Irecv(xrecvall[0],3*neb_modatoms,MPI_DOUBLE,procprev,0,uworld,&requests[0]);
    MPI_Irecv(tagrecvall,neb_modatoms,MPI_LMP_TAGINT,procprev,0,uworld,
              &requests[1]);
  }
  if (ireplica < nreplica-1 && me == 0) {
    MPI_Send(xsendall[0],3*neb_modatoms,MPI_DOUBLE,procnext,0,uworld);
    MPI_Send(tagsendall,neb_modatoms,MPI_LMP_TAGINT,procnext,0,uworld);
  }

  if (ireplica > 0) {
    if (me == 0) MPI_Waitall(2,requests,statuses);

    MPI_Bcast(tagrecvall,neb_modatoms,MPI_INT,0,world);
    MPI_Bcast(xrecvall[0],3*neb_modatoms,MPI_DOUBLE,0,world);

    for (i = 0; i < neb_modatoms; i++) {
      m = atom->map(tagrecvall[i]);
      if (m < 0 || m >= nlocal) continue;
      xprev[m][0] = xrecvall[i][0];
      xprev[m][1] = xrecvall[i][1];
      xprev[m][2] = xrecvall[i][2];
    }
  }

  if (ireplica < nreplica-1 && me == 0) {
    MPI_Irecv(xrecvall[0],3*neb_modatoms,MPI_DOUBLE,procnext,0,uworld,&requests[0]);
    MPI_Irecv(frecvall[0],3*neb_modatoms,MPI_DOUBLE,procnext,0,uworld,&requests[0]);
    MPI_Irecv(tagrecvall,neb_modatoms,MPI_LMP_TAGINT,procnext,0,uworld,
              &requests[1]);
  }
  if (ireplica > 0 && me == 0) {
    MPI_Send(xsendall[0],3*neb_modatoms,MPI_DOUBLE,procprev,0,uworld);
    MPI_Send(fsendall[0],3*neb_modatoms,MPI_DOUBLE,procprev,0,uworld);
    MPI_Send(tagsendall,neb_modatoms,MPI_LMP_TAGINT,procprev,0,uworld);
  }

  if (ireplica < nreplica-1) {
    if (me == 0) MPI_Waitall(2,requests,statuses);

    MPI_Bcast(tagrecvall,neb_modatoms,MPI_INT,0,world);
    MPI_Bcast(xrecvall[0],3*neb_modatoms,MPI_DOUBLE,0,world);
    MPI_Bcast(frecvall[0],3*neb_modatoms,MPI_DOUBLE,0,world);

    for (i = 0; i < neb_modatoms; i++) {
      m = atom->map(tagrecvall[i]);
      if (m < 0 || m >= nlocal) continue;
      xnext[m][0] = xrecvall[i][0];
      xnext[m][1] = xrecvall[i][1];
      xnext[m][2] = xrecvall[i][2];
      fnext[m][0] = frecvall[i][0];
      fnext[m][1] = frecvall[i][1];
      fnext[m][2] = frecvall[i][2];
    }
  }
}

/* ----------------------------------------------------------------------
   reallocate xprev,xnext,tangent arrays if necessary
   reallocate communication arrays if necessary
------------------------------------------------------------------------- */

void FixNEB_MOD::reallocate()
{
  maxlocal = atom->nmax;

  memory->destroy(xprev);
  memory->destroy(xnext);
  memory->destroy(tangent);
  memory->destroy(fnext);
  memory->destroy(springF);

  memory->create(xprev,maxlocal,3,"neb_mod:xprev");
  memory->create(xnext,maxlocal,3,"neb_mod:xnext");
  memory->create(tangent,maxlocal,3,"neb_mod:tangent");
  memory->create(fnext,maxlocal,3,"neb_mod:fnext");
  memory->create(springF,maxlocal,3,"neb_mod:springF");

  if (cmode != SINGLE_PROC_DIRECT) {
    memory->destroy(xsend);
    memory->destroy(fsend);
    memory->destroy(xrecv);
    memory->destroy(frecv);
    memory->destroy(tagsend);
    memory->destroy(tagrecv);
    memory->create(xsend,maxlocal,3,"neb_mod:xsend");
    memory->create(fsend,maxlocal,3,"neb_mod:fsend");
    memory->create(xrecv,maxlocal,3,"neb_mod:xrecv");
    memory->create(frecv,maxlocal,3,"neb_mod:frecv");
    memory->create(tagsend,maxlocal,"neb_mod:tagsend");
    memory->create(tagrecv,maxlocal,"neb_mod:tagrecv");
  }

  if (NEB_MODLongRange || AutomaticUpdateKspring) {
    memory->destroy(nlenall);
    memory->create(nlenall,nreplica,"neb_mod:nlenall");
  }
  if (AutomaticUpdateKspring) {
    memory->destroy(freplicaall);
    memory->create(freplicaall, nreplica, "neb_mod:freplicaall");
  }
}
