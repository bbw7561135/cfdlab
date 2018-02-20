static char help[] = "Solves 2d Euler equations.\n\n";

#include <petscdm.h>
#include <petscdmda.h>
#include <petscts.h>

enum bctype { wall, periodic, farfield, supersonic };

#include "isentropic.h"

// Number of variables at each grid point
#define nvar  4

const PetscInt sw = 3; // stencil width, 3 on either side, for weno5
double dx, dy;

typedef struct
{
   PetscReal dt, cfl, Tf;
   PetscInt  max_steps, si;
   Vec       fxp, fxm, fyp, fym;
} AppCtx;

extern PetscErrorCode RHSFunction(TS,PetscReal,Vec,Vec,void*);
extern PetscErrorCode Monitor(TS,PetscInt,PetscReal,Vec,void*);

//------------------------------------------------------------------------------
// Weno reconstruction
// Return left value for face between u0, up1
//------------------------------------------------------------------------------
double weno5(const double um2, const double um1, const double u0, const double up1, const double up2)
{
   double eps = 1.0e-6;
   double gamma1=1.0/10.0, gamma2=3.0/5.0, gamma3=3.0/10.0;
   double beta1, beta2, beta3;
   double u1, u2, u3;
   double w1, w2, w3;

   beta1 = (13.0/12.0)*pow((um2 - 2.0*um1 + u0),2) +
           (1.0/4.0)*pow((um2 - 4.0*um1 + 3.0*u0),2);
   beta2 = (13.0/12.0)*pow((um1 - 2.0*u0 + up1),2) +
           (1.0/4.0)*pow((um1 - up1),2);
   beta3 = (13.0/12.0)*pow((u0 - 2.0*up1 + up2),2) +
           (1.0/4.0)*pow((3.0*u0 - 4.0*up1 + up2),2);

   w1 = gamma1 / pow(eps+beta1, 2);
   w2 = gamma2 / pow(eps+beta2, 2);
   w3 = gamma3 / pow(eps+beta3, 2);

   u1 = (1.0/3.0)*um2 - (7.0/6.0)*um1 + (11.0/6.0)*u0;
   u2 = -(1.0/6.0)*um1 + (5.0/6.0)*u0 + (1.0/3.0)*up1;
   u3 = (1.0/3.0)*u0 + (5.0/6.0)*up1 - (1.0/6.0)*up2;

   return (w1 * u1 + w2 * u2 + w3 * u3)/(w1 + w2 + w3);
}

// Conserved to primitive variables
void con2prim(const double *Con, double *Prim)
{
  Prim[0] = Con[0];
  Prim[1] = Con[1]/Con[0];
  Prim[2] = Con[2]/Con[0];
  Prim[3] = (Con[3] - 0.5*Prim[0]*(pow(Prim[1],2) + pow(Prim[2],2)))*(gas_gamma-1.0);
}

// Primitive to conserved variables
void prim2con(const double *Prim, double *Con)
{
  Con[0] = Prim[0];
  Con[1] = Prim[0]*Prim[1];
  Con[2] = Prim[0]*Prim[2];
  Con[3] = 0.5*Prim[0]*(pow(Prim[1],2) + pow(Prim[2],2)) + Prim[3]/(gas_gamma-1.0);
}

// Compute max eigenvalue along x and y
void compute_lambda(const double *Con, double *lambdax, double *lambday)
{
   double vx = Con[1]/Con[0];
   double vy = Con[2]/Con[0];
   double p = (gas_gamma - 1.0) * (Con[3] - 0.5*Con[0]*(vx*vx + vy*vy));
   double a = sqrt(gas_gamma * p / Con[0]);

   *lambdax = fabs(vx) + a;
   *lambday = fabs(vy) + a;
}

// Compute local timestep
double dt_local(const double *Con)
{
   double Prim[nvar];
   con2prim(Con, Prim);
   const double a = sqrt(gas_gamma*Prim[3]/Prim[0]);
   const double sx = fabs(Prim[1]) + a;
   const double sy = fabs(Prim[2]) + a;
   return 1.0/(sx/dx + sy/dy);
}

// Computes split fluxes along direction (nx,ny)
void split_fluxes(const double *U, const double nx, const double ny,
                  const double lambda, double *fp, double *fm)
{
   double P[nvar], flux[nvar];
   con2prim(U, P);
   flux[0] = U[1]*nx + U[2]*ny;
   flux[1] = P[3]*nx + P[1]*flux[0];
   flux[2] = P[3]*ny + P[2]*flux[0];
   flux[3] = (U[3] + P[3]) * (P[1]*nx + P[2]*ny);

   for(int i=0; i<nvar; ++i)
   {
      fp[i] = 0.5*(flux[i] + lambda * U[i]);
      fm[i] = 0.5*(flux[i] - lambda * U[i]);
   }
}

//------------------------------------------------------------------------------
PetscErrorCode savesol(double t, DM da, Vec ug)
{
   PetscErrorCode ierr;
   char           filename[32] = "sol";
   PetscMPIInt    rank;
   PetscInt       i, j, nx, ny, ibeg, jbeg, nlocx, nlocy;
   FILE           *fp;
   Vec            ul;
   PetscScalar    ***u;
   static int     c = 0;

   ierr = DMGetLocalVector(da, &ul); CHKERRQ(ierr);
   ierr = DMGlobalToLocalBegin(da, ug, INSERT_VALUES, ul); CHKERRQ(ierr);
   ierr = DMGlobalToLocalEnd(da, ug, INSERT_VALUES, ul); CHKERRQ(ierr);
   ierr = DMDAVecGetArrayDOFRead(da, ul, &u); CHKERRQ(ierr);
   ierr = DMDAGetInfo(da,0,&nx,&ny,0,0,0,0,0,0,0,0,0,0); CHKERRQ(ierr);
   ierr = DMDAGetCorners(da, &ibeg, &jbeg, 0, &nlocx, &nlocy, 0); CHKERRQ(ierr);

   int iend = PetscMin(ibeg+nlocx+1, nx);
   int jend = PetscMin(jbeg+nlocy+1, ny);

   MPI_Comm_rank(PETSC_COMM_WORLD, &rank);
   sprintf(filename, "sol-%03d-%03d.plt", c, rank);
   fp = fopen(filename,"w");
   fprintf(fp, "TITLE = \"u_t + u_x + u_y = 0\"\n");
   fprintf(fp, "VARIABLES = x, y, rho, u, v, p\n");
   fprintf(fp, "ZONE STRANDID=1, SOLUTIONTIME=%e, I=%d, J=%d, DATAPACKING=POINT\n", t, iend-ibeg, jend-jbeg);
   for(j=jbeg; j<jend; ++j)
      for(i=ibeg; i<iend; ++i)
   {
      PetscReal x = xmin + i*dx + 0.5*dx;
      PetscReal y = ymin + j*dy + 0.5*dy;
      double prim[nvar];
      con2prim(u[j][i], prim);
      fprintf(fp, "%e %e %e %e %e %e\n", x, y, prim[0], prim[1], prim[2], prim[3]);
   }
   fclose(fp);

   ierr = DMDAVecRestoreArrayDOFRead(da, ul, &u); CHKERRQ(ierr);
   ierr = DMRestoreLocalVector(da, &ul); CHKERRQ(ierr);

   ++c;
   return(0);
}
//------------------------------------------------------------------------------
int main(int argc, char *argv[])
{
   // some parameters that can overwritten from command line
   PetscInt  nx  = 50, ny=50; // use -da_grid_x, -da_grid_y to override these
   
   PetscErrorCode ierr;
   AppCtx      ctx;
   TS          ts;
   DM          da;
   Vec         ug;
   PetscInt    i, j, ibeg, jbeg, nlocx, nlocy;
   PetscMPIInt rank, size;
   PetscReal   dtglobal, dtlocal = 1.0e-20;
   PetscScalar ***u;

   ierr = PetscInitialize(&argc, &argv, (char*)0, help); CHKERRQ(ierr);

   ctx.Tf  = 10.0;
   ctx.dt  = -1.0;
   ctx.cfl = -1.0;
   ctx.max_steps = 1000000;
   ctx.si = 100;

   MPI_Comm_rank(PETSC_COMM_WORLD, &rank);
   MPI_Comm_size(PETSC_COMM_WORLD, &size);
   
   // Get some command line options
   ierr = PetscOptionsGetReal(NULL,NULL,"-Tf",&ctx.Tf,NULL); CHKERRQ(ierr);
   ierr = PetscOptionsGetReal(NULL,NULL,"-dt",&ctx.dt,NULL); CHKERRQ(ierr);
   ierr = PetscOptionsGetReal(NULL,NULL,"-cfl",&ctx.cfl,NULL); CHKERRQ(ierr);
   ierr = PetscOptionsGetInt(NULL,NULL,"-si",&ctx.si,NULL); CHKERRQ(ierr);

   if(PERIODIC == 0) // periodic in x
   {
      ierr = DMDACreate2d(PETSC_COMM_WORLD, DM_BOUNDARY_PERIODIC, DM_BOUNDARY_GHOSTED,
                          DMDA_STENCIL_BOX, nx, ny, PETSC_DECIDE, PETSC_DECIDE, nvar,
                          sw, NULL, NULL, &da); CHKERRQ(ierr);
      ierr = PetscPrintf(PETSC_COMM_WORLD,"Periodic in x\n"); CHKERRQ(ierr);
   }
   else if(PERIODIC == 1) // periodic in y
   {
      ierr = DMDACreate2d(PETSC_COMM_WORLD, DM_BOUNDARY_GHOSTED, DM_BOUNDARY_PERIODIC,
                          DMDA_STENCIL_BOX, nx, ny, PETSC_DECIDE, PETSC_DECIDE, nvar,
                          sw, NULL, NULL, &da); CHKERRQ(ierr);
      ierr = PetscPrintf(PETSC_COMM_WORLD,"Periodic in y\n"); CHKERRQ(ierr);
   }
   else if(PERIODIC == 2) // periodic in both x and y
   {
      ierr = DMDACreate2d(PETSC_COMM_WORLD, DM_BOUNDARY_PERIODIC, DM_BOUNDARY_PERIODIC,
                          DMDA_STENCIL_BOX, nx, ny, PETSC_DECIDE, PETSC_DECIDE, nvar,
                          sw, NULL, NULL, &da); CHKERRQ(ierr);
      ierr = PetscPrintf(PETSC_COMM_WORLD,"Periodic in x and y\n"); CHKERRQ(ierr);
   }
   else // no periodic bc
   {
      ierr = DMDACreate2d(PETSC_COMM_WORLD, DM_BOUNDARY_GHOSTED, DM_BOUNDARY_GHOSTED,
                          DMDA_STENCIL_BOX, nx, ny, PETSC_DECIDE, PETSC_DECIDE, nvar,
                          sw, NULL, NULL, &da); CHKERRQ(ierr);
   }
   ierr = DMSetFromOptions(da); CHKERRQ(ierr);
   ierr = DMSetUp(da); CHKERRQ(ierr);

   ierr = DMDAGetInfo(da,0,&nx,&ny,0,0,0,0,0,0,0,0,0,0); CHKERRQ(ierr);
   dx = (xmax - xmin) / (PetscReal)(nx);
   dy = (ymax - ymin) / (PetscReal)(ny);
   PetscPrintf(PETSC_COMM_WORLD,"nx = %d, dx = %e\n", nx, dx);
   PetscPrintf(PETSC_COMM_WORLD,"ny = %d, dy = %e\n", ny, dy);

   ierr = DMCreateGlobalVector(da, &ug); CHKERRQ(ierr);
   ierr = PetscObjectSetName((PetscObject) ug, "Solution"); CHKERRQ(ierr);

   // Create vectors to store split fluxes
   ierr = DMCreateLocalVector(da, &ctx.fxp); CHKERRQ(ierr);
   ierr = DMCreateLocalVector(da, &ctx.fxm); CHKERRQ(ierr);
   ierr = DMCreateLocalVector(da, &ctx.fyp); CHKERRQ(ierr);
   ierr = DMCreateLocalVector(da, &ctx.fym); CHKERRQ(ierr);

   ierr = DMDAGetCorners(da, &ibeg, &jbeg, 0, &nlocx, &nlocy, 0); CHKERRQ(ierr);
   ierr = DMDAVecGetArrayDOF(da, ug, &u); CHKERRQ(ierr);
   for(j=jbeg; j<jbeg+nlocy; ++j)
      for(i=ibeg; i<ibeg+nlocx; ++i)
      {
         PetscReal x = xmin + i*dx + 0.5*dx;
         PetscReal y = ymin + j*dy + 0.5*dy;
         PetscReal prim[nvar];
         initcond(x, y, prim);
         prim2con(prim, u[j][i]);
         dtlocal = PetscMin(dtlocal, dt_local(u[j][i]));
      }
   ierr = DMDAVecRestoreArrayDOF(da, ug, &u); CHKERRQ(ierr);
   MPI_Allreduce(&dtlocal, &dtglobal, 1, MPI_DOUBLE, MPI_MIN, PETSC_COMM_WORLD);
   if(ctx.cfl > 0)
   {
      ctx.dt = ctx.cfl * dtglobal;
      PetscPrintf(PETSC_COMM_WORLD,"Using dt based on specified cfl\n");
   }
   else if(ctx.dt > 0)
   {
      PetscPrintf(PETSC_COMM_WORLD,"Global dt = %e\n", dtglobal);
      PetscPrintf(PETSC_COMM_WORLD,"Using specified dt\n");
   }
   else
   {
      PetscPrintf(PETSC_COMM_WORLD,"Specify atleast dt or cfl\n");
      return(0);
   }
   PetscPrintf(PETSC_COMM_WORLD,"Initial time step = %e\n", ctx.dt);

   // Save initial condition to file
   ierr = savesol(0.0, da, ug); CHKERRQ(ierr);

   ierr = TSCreate(PETSC_COMM_WORLD,&ts); CHKERRQ(ierr);
   ierr = TSSetDM(ts,da); CHKERRQ(ierr);
   ierr = TSSetProblemType(ts,TS_NONLINEAR); CHKERRQ(ierr);
   ierr = TSSetRHSFunction(ts,NULL,RHSFunction,&ctx); CHKERRQ(ierr);
   ierr = TSSetTimeStep(ts,ctx.dt);
   ierr = TSSetType(ts,TSSSP); CHKERRQ(ierr);
   ierr = TSSetMaxSteps(ts,ctx.max_steps); CHKERRQ(ierr);
   ierr = TSSetMaxTime(ts,ctx.Tf); CHKERRQ(ierr);
   ierr = TSSetExactFinalTime(ts,TS_EXACTFINALTIME_MATCHSTEP); CHKERRQ(ierr);
   ierr = TSSetSolution(ts,ug); CHKERRQ(ierr);
   ierr = TSMonitorSet(ts,Monitor,&ctx,NULL); CHKERRQ(ierr);
   ierr = TSSetFromOptions(ts); CHKERRQ(ierr);
   ierr = TSSetUp(ts); CHKERRQ(ierr);

   ierr = TSSolve(ts,ug); CHKERRQ(ierr);

   // Destroy everything before finishing
   ierr = VecDestroy(&ug); CHKERRQ(ierr);
   ierr = VecDestroy(&ctx.fxp); CHKERRQ(ierr);
   ierr = VecDestroy(&ctx.fxm); CHKERRQ(ierr);
   ierr = VecDestroy(&ctx.fyp); CHKERRQ(ierr);
   ierr = VecDestroy(&ctx.fym); CHKERRQ(ierr);

   ierr = DMDestroy(&da); CHKERRQ(ierr);
   ierr = TSDestroy(&ts); CHKERRQ(ierr);

   ierr = PetscFinalize(); CHKERRQ(ierr);
}

// The rhs function in du/dt = R(t,u)
PetscErrorCode RHSFunction(TS ts,PetscReal time,Vec U,Vec R,void* ptr)
{
   AppCtx*        ctx = (AppCtx*) ptr;
   DM             da;
   Vec            localU;
   PetscScalar    ***u;
   PetscScalar    ***res;
   PetscScalar    ***fxp;
   PetscScalar    ***fxm;
   PetscScalar    ***fyp;
   PetscScalar    ***fym;
   PetscInt       i, j, ibeg, jbeg, nlocx, nlocy, d, nx, ny;
   PetscReal      UL[nvar], UR[nvar], flux[nvar], lam, lamx, lamy, lambdax, lambday;
   PetscErrorCode ierr;

   ierr = TSGetDM(ts, &da); CHKERRQ(ierr);
   ierr = DMGetLocalVector(da,&localU); CHKERRQ(ierr);
   ierr = DMGlobalToLocalBegin(da, U, INSERT_VALUES, localU); CHKERRQ(ierr);
   ierr = DMGlobalToLocalEnd(da, U, INSERT_VALUES, localU); CHKERRQ(ierr);
   ierr = DMDAVecGetArrayDOF(da, localU, &u); CHKERRQ(ierr);
   ierr = DMDAVecGetArrayDOF(da, R, &res); CHKERRQ(ierr);

   ierr = DMDAVecGetArrayDOF(da, ctx->fxp, &fxp); CHKERRQ(ierr);
   ierr = DMDAVecGetArrayDOF(da, ctx->fxm, &fxm); CHKERRQ(ierr);
   ierr = DMDAVecGetArrayDOF(da, ctx->fyp, &fyp); CHKERRQ(ierr);
   ierr = DMDAVecGetArrayDOF(da, ctx->fym, &fym); CHKERRQ(ierr);

   ierr = DMDAGetInfo(da,0,&nx,&ny,0,0,0,0,0,0,0,0,0,0); CHKERRQ(ierr);
   ierr = DMDAGetCorners(da, &ibeg, &jbeg, 0, &nlocx, &nlocy, 0); CHKERRQ(ierr);

   // ---Begin res computation---
   // Set residual 0
   for(j=jbeg; j<jbeg+nlocy; ++j)
      for(i=ibeg; i<ibeg+nlocx; ++i)
         for(d=0; d<nvar; ++d)
            res[j][i][d] = 0;

   // Fill in ghost values based on boundary condition
   // Left side
   if(ibeg == 0 && BC_LEFT == wall)
   {
      i = ibeg - 1;
      for(j=jbeg; j<jbeg+nlocy; ++j)
      {
         u[j][i][0] =  u[j][i+1][0];
         u[j][i][1] = -u[j][i+1][1];
         u[j][i][2] =  u[j][i+1][2];
         u[j][i][3] =  u[j][i+1][3];

         u[j][i-1][0] =  u[j][i+2][0];
         u[j][i-1][1] = -u[j][i+2][1];
         u[j][i-1][2] =  u[j][i+2][2];
         u[j][i-1][3] =  u[j][i+2][3];

         u[j][i-2][0] =  u[j][i+3][0];
         u[j][i-2][1] = -u[j][i+3][1];
         u[j][i-2][2] =  u[j][i+3][2];
         u[j][i-2][3] =  u[j][i+3][3];
      }

   }
   else if(ibeg == 0 && BC_LEFT == farfield)
   {
      SETERRQ(PETSC_COMM_WORLD,1,"Not implemented");
   }

   // Right side
   if(ibeg+nlocx == nx && BC_RIGHT == wall)
   {
      i = ibeg + nlocx;
      for(j=jbeg; j<jbeg+nlocy; ++j)
      {
         u[j][i][0] =  u[j][i-1][0];
         u[j][i][1] = -u[j][i-1][1];
         u[j][i][2] =  u[j][i-1][2];
         u[j][i][3] =  u[j][i-1][3];

         u[j][i+1][0] =  u[j][i-2][0];
         u[j][i+1][1] = -u[j][i-2][1];
         u[j][i+1][2] =  u[j][i-2][2];
         u[j][i+1][3] =  u[j][i-2][3];

         u[j][i+2][0] =  u[j][i-3][0];
         u[j][i+2][1] = -u[j][i-3][1];
         u[j][i+2][2] =  u[j][i-3][2];
         u[j][i+2][3] =  u[j][i-3][3];
      }

   }
   else if(BC_RIGHT == farfield)
   {
      SETERRQ(PETSC_COMM_WORLD,1,"Not implemented");
   }

   // Bottom side
   if(jbeg == 0 && BC_BOTTOM == wall)
   {
      j = jbeg - 1;
      for(i=ibeg; i<ibeg+nlocx; ++i)
      {
         u[j][i][0] =  u[j+1][i][0];
         u[j][i][1] =  u[j+1][i][1];
         u[j][i][2] = -u[j+1][i][2];
         u[j][i][3] =  u[j+1][i][3];

         u[j-1][i][0] =  u[j+2][i][0];
         u[j-1][i][1] =  u[j+2][i][1];
         u[j-1][i][2] = -u[j+2][i][2];
         u[j-1][i][3] =  u[j+2][i][3];

         u[j-2][i][0] =  u[j+3][i][0];
         u[j-2][i][1] =  u[j+3][i][1];
         u[j-2][i][2] = -u[j+3][i][2];
         u[j-2][i][3] =  u[j+3][i][3];
      }

   }
   else if(BC_BOTTOM == farfield)
   {
      SETERRQ(PETSC_COMM_WORLD,1,"Not implemented");
   }

   // Top side
   if(jbeg+nlocy == ny && BC_TOP == wall)
   {
      j = jbeg + nlocy;
      for(i=ibeg; i<ibeg+nlocx; ++i)
      {
         u[j][i][0] =  u[j-1][i][0];
         u[j][i][1] =  u[j-1][i][1];
         u[j][i][2] = -u[j-1][i][2];
         u[j][i][3] =  u[j-1][i][3];

         u[j+1][i][0] =  u[j-2][i][0];
         u[j+1][i][1] =  u[j-2][i][1];
         u[j+1][i][2] = -u[j-2][i][2];
         u[j+1][i][3] =  u[j-2][i][3];

         u[j+2][i][0] =  u[j-3][i][0];
         u[j+2][i][1] =  u[j-3][i][1];
         u[j+2][i][2] = -u[j-3][i][2];
         u[j+2][i][3] =  u[j-3][i][3];
      }

   }
   else if(BC_TOP == farfield)
   {
      SETERRQ(PETSC_COMM_WORLD,1,"Not implemented");
   }

   // compute maximum wave speeds
   for(j=jbeg; j<jbeg+nlocy; ++j)
      for(i=ibeg; i<ibeg+nlocx; ++i)
      {
         compute_lambda(u[j][i], &lamx, &lamy);
         lambdax = PetscMax(lambdax, lamx);
         lambday = PetscMax(lambday, lamy);
      }
   lamx = lambdax; lamy = lambday;
   MPI_Allreduce(&lamx, &lambdax, 1, MPI_DOUBLE, MPI_MAX, PETSC_COMM_WORLD);
   MPI_Allreduce(&lamy, &lambday, 1, MPI_DOUBLE, MPI_MAX, PETSC_COMM_WORLD);

   // Compute x-split fluxes
   for(j=jbeg; j<jbeg+nlocy; ++j)
      for(i=ibeg-sw; i<ibeg+nlocx+sw; ++i)
      {
         split_fluxes(u[j][i], 1.0, 0.0, lambdax, fxp[j][i], fxm[j][i]);
      }

   // Compute y-split fluxes
   for(j=jbeg-sw; j<jbeg+nlocy+sw; ++j)
      for(i=ibeg; i<ibeg+nlocx; ++i)
      {
         split_fluxes(u[j][i], 0.0, 1.0, lambday, fyp[j][i], fym[j][i]);
      }

   // x fluxes
   for(j=jbeg; j<jbeg+nlocy; ++j)
      for(i=ibeg; i<ibeg+nlocx+1; ++i)
      {
         // Compute average state
         // Compute eigenvector matrix
         // Transform split fluxes
         
         // face between i-1, i
         for(d=0; d<nvar; ++d)
         {
            UL[d] = weno5(fxp[j][i-3][d],fxp[j][i-2][d],fxp[j][i-1][d],fxp[j][i][d],fxp[j][i+1][d]);
            UR[d] = weno5(fxm[j][i+2][d],fxm[j][i+1][d],fxm[j][i][d],fxm[j][i-1][d],fxm[j][i-2][d]);
         }
         if(i==ibeg)
         {
            for(d=0; d<nvar; ++d)
               res[j][i][d] -= dy * flux[d];
         }
         else if(i==ibeg+nlocx)
         {
            for(d=0; d<nvar; ++d)
               res[j][i-1][d] += dy * flux[d];
         }
         else
         {
            for(d=0; d<nvar; ++d)
            {
               res[j][i][d]   -= dy * flux[d];
               res[j][i-1][d] += dy * flux[d];
            }
         }
      }

   // y fluxes
   for(j=jbeg; j<jbeg+nlocy+1; ++j)
      for(i=ibeg; i<ibeg+nlocx; ++i)
      {
         // face between j-1, j
         for(d=0; d<nvar; ++d)
         {
            UL[d] = weno5(fyp[j-3][i][d],fyp[j-2][i][d],fyp[j-1][i][d],fyp[j][i][d],fyp[j+1][i][d]);
            UR[d] = weno5(fym[j+2][i][d],fym[j+1][i][d],fym[j][i][d],fym[j-1][i][d],fym[j-2][i][d]);
         }
         if(j==jbeg)
         {
            for(d=0; d<nvar; ++d)
               res[j][i][d] -= dx * flux[d];
         }
         else if(j==jbeg+nlocy)
         {
            for(d=0; d<nvar; ++d)
               res[j-1][i][d] += dx * flux[d];
         }
         else
         {
            for(d=0; d<nvar; ++d)
            {
               res[j][i][d]   -= dx * flux[d];
               res[j-1][i][d] += dx * flux[d];
            }
         }
      }

   lam = 1.0/(dx*dy);
   for(j=jbeg; j<jbeg+nlocy; ++j)
      for(i=ibeg; i<ibeg+nlocx; ++i)
         for(d=0; d<nvar; ++d)
            res[j][i][d] *= -lam;
   // ---End res computation---

   ierr = DMDAVecRestoreArrayDOF(da, localU, &u); CHKERRQ(ierr);
   ierr = DMDAVecRestoreArrayDOF(da, R, &res); CHKERRQ(ierr);
   ierr = DMRestoreLocalVector(da,&localU); CHKERRQ(ierr);

   ierr = DMDAVecRestoreArrayDOF(da, ctx->fxp, &fxp); CHKERRQ(ierr);
   ierr = DMDAVecRestoreArrayDOF(da, ctx->fxm, &fxm); CHKERRQ(ierr);
   ierr = DMDAVecRestoreArrayDOF(da, ctx->fyp, &fyp); CHKERRQ(ierr);
   ierr = DMDAVecRestoreArrayDOF(da, ctx->fym, &fym); CHKERRQ(ierr);

   PetscFunctionReturn(0);
}

// This function is called after every time step.
PetscErrorCode Monitor(TS ts,PetscInt step,PetscReal time,Vec U,void *ptr)
{
   AppCtx*        ctx = (AppCtx*) ptr;
   DM             da;
   PetscInt       i, j, ibeg, jbeg, nlocx, nlocy;
   PetscReal      dtlocal, dtglobal;
   PetscScalar    ***u;
   PetscErrorCode ierr;

   if (step < 0) return(0); /* step of -1 indicates an interpolated solution */
   PetscPrintf(PETSC_COMM_WORLD,"iter, t = %e\n", step, time);

   ierr = TSGetDM(ts, &da); CHKERRQ(ierr);

   if(step%ctx->si == 0 || PetscAbs(time-ctx->Tf) < 1.0e-13)
   {
      ierr = savesol(time, da, U); CHKERRQ(ierr);
   }

   // If final time reached, dont do anything else, return from function.
   if(PetscAbs(time-ctx->Tf) < 1.0e-13)
      PetscFunctionReturn(0);

   // Compute time step based on cfl
   if(ctx->cfl > 0)
   {
      ierr = DMDAGetCorners(da, &ibeg, &jbeg, 0, &nlocx, &nlocy, 0); CHKERRQ(ierr);
      ierr = DMDAVecGetArrayDOFRead(da, U, &u); CHKERRQ(ierr);

      dtlocal = 1.0e20;
      for(j=jbeg; j<jbeg+nlocy; ++j)
         for(i=ibeg; i<ibeg+nlocx; ++i)
         {
            dtlocal = PetscMin(dtlocal, dt_local(u[j][i]));
         }
      ierr = DMDAVecRestoreArrayDOFRead(da, U, &u); CHKERRQ(ierr);
      MPI_Allreduce(&dtlocal, &dtglobal, 1, MPI_DOUBLE, MPI_MIN, PETSC_COMM_WORLD);
      dtglobal *= ctx->cfl;
      // Adjust dt to reach final time exactly
      if(time+dtglobal > ctx->Tf) dtglobal = ctx->Tf - time;
      ierr = TSSetTimeStep(ts, dtglobal); CHKERRQ(ierr);
   }

   PetscFunctionReturn(0);
}
