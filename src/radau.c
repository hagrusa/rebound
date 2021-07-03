// TES is an open source integration package for modelling exoplanet evolution.
// Copyright (C) <2021>  <Peter Bartram, Alexander Wittig>

// TES is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.

// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.

// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <https://www.gnu.org/licenses/>. 

#include "radau.h"
#include "radau_step.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdio.h>
#include "terminate.h"
#include <time.h>
#include "simulation.h"
#include "dhem.h"
#include "UniversalVars.h"

#define FINAL_STAGE_INDEX 8
#define MAX_STEP_SIZE_GROWTH 4.0
#define MIN_STEP_SIZE 1E-5
#define OSCULATING_ORBIT_SLOTS 9  // stages + 2 for t=0 and t=1.

static RADAU * radau;
static SIMULATION * sim;

double t = 0.0;
double tEnd = 0.0;
double h = 0.0;
uint32_t iterations = 0;

static void OutputToFile(double t, uint32_t rectified, uint32_t iterations, double h, uint32_t forceOutput);
static double hArr[9] = {0.0, 0.0562625605369221464656521910318, 0.180240691736892364987579942780, 0.352624717113169637373907769648, 0.547153626330555383001448554766, 0.734210177215410531523210605558, 0.885320946839095768090359771030, 0.977520613561287501891174488626, 1.0};


double Radau_SingleStep(double z_t, double dt, double dt_last_done)
{
    double dt_new = 0.0;
    radau->h = dt;
    radau->t = z_t;

    OutputToFile(z_t, sim->rectificationCount, iterations, dt, 0);

    sim->rectificationCount = sim->fRectify(z_t, sim->Q_dh, sim->P_dh, radau->dQ,
                                        radau->dP, radau->rectifiedArray, FINAL_STAGE_INDEX);
    radau->rectifications += sim->rectificationCount;

    // Calculate the osculating orbits.
    sim->fStartOfStep(z_t, dt, hArr, OSCULATING_ORBIT_SLOTS, 1);

    ClearRectifiedBFields(radau->B, radau->rectifiedArray);
    ClearRectifiedBFields(radau->B_1st, radau->rectifiedArray);

    radau->CalculateGfromB(); 

    radau->step(&iterations, z_t, dt, sim->step);
    radau->convergenceIterations += iterations;

    dt_new = sim->rTol > 0 ? Radau_CalculateStepSize(dt, dt_last_done, z_t) : dt;

    radau->AnalyticalContinuation(radau->B_1st, radau->Blast_1st, dt, dt_new, radau->rectifiedArray, sim->step);
    radau->AnalyticalContinuation(radau->B, radau->Blast, dt, dt_new, radau->rectifiedArray, sim->step);
  
    // for(uint32_t i = 0; i < 3; i++)
    // {
    //   printf("\n%.8E %.8E %.8E", radau->dQ[3*i], radau->dQ[3*i+1], radau->dQ[3*i+2]);
    // }
    return dt_new;
}

uint32_t Radau_integrate(void)
{
  sim->step = 0;
  sim->rectificationCount = 0;
  t = sim->t0;
  tEnd = sim->tEnd;
  h = sim->hInitial;

  radau->tStart = clock();

  sim->H0 = sim->fCalculateInvariant(sim->Q_dh, sim->P_dh);

  while(1)
  {
    double hNew = 0.0;

    hNew = Radau_SingleStep(t, h, sim->h_last_done);
    // printf("\nt: %.5f. dt: %.5f. dt_new: %.5f", t, h, hNew);

    t += h; 
    
    if(fabs(t-tEnd) < 1E-15 || t > tEnd)
    {
      OutputToFile(t, sim->rectificationCount, iterations, h, 1);
      // We have finished
      break;
    }

    // Final step should usually be a smaller one to ensure correct end time.
    if(t+hNew > tEnd)
    {
      hNew = tEnd-t;
    }

    sim->step++;
    sim->h_last_done = h;
    // Updating the step size should be the final thing we do.
    h = hNew;
  }

  radau->stepsTaken = sim->step;

  sim->orbits = t / sim->period;
  sim->tEnd = t;
  clock_t tFinish = clock();
  radau->cpuTimeUsed = ((double)(tFinish - radau->tStart)) / CLOCKS_PER_SEC;

  sim->H1 = sim->fCalculateInvariant(sim->Q_dh, sim->P_dh);
  printf("\nH1: %.15E", sim->H1);
  printf("\n\nIntegration Statistics:\n");
  printf("Orbits: %f\n", sim->orbits);
  printf("Period: %.16f\n", sim->period);
  printf("End time: %.16f\n", sim->tEnd);
  printf("Tolerance: %.2E\n", sim->rTol);
  printf("Steps taken: %ld\n", radau->stepsTaken);
  printf("Steps per orbit: %f\n", radau->stepsTaken/sim->orbits);
  printf("Function calls (per orbit): %.2f\n", radau->fCalls/sim->orbits);
  printf("Runtime: %f s\n", radau->cpuTimeUsed);
  printf("Iterations per step: %f\n", (double)radau->convergenceIterations/(double)radau->stepsTaken);
  printf("Rectifications per orbit: %f\n", (double)radau->rectifications/((double)sim->orbits*(sim->n-1)));
  printf("Change in hamiltonian: %E\n", fabs((sim->H1-sim->H0)/sim->H0));

  return 1;
}

void Radau_Init(SIMULATION * z_sim)
{
  sim = z_sim;
  radau = (RADAU *)malloc(sizeof(RADAU));
  memset(radau, 0, sizeof(RADAU));

  sim->radau = radau;
  radau->dX = (double*)malloc(sim->stateVectorSize);
  radau->dXtemp = (double*)malloc(sim->stateVectorSize);
  radau->dX0 = (double*)malloc(sim->stateVectorSize);
  radau->X = (double*)malloc(sim->stateVectorSize);
  radau->Xout = (double*)malloc(sim->stateVectorSize);
  radau->predictors = (double*)malloc(sim->stateVectorSize);

  radau->q = (double*)malloc(sim->stateVectorSize/2);
  radau->p = (double*)malloc(sim->stateVectorSize/2);
  radau->q_dot = (double*)malloc(sim->stateVectorSize/2);
  radau->q_ddot = (double*)malloc(sim->stateVectorSize/2);
  radau->p_dot = (double*)malloc(sim->stateVectorSize/2);

  memset(radau->q, 0, sim->stateVectorSize/2);
  memset(radau->p, 0, sim->stateVectorSize/2);
  memset(radau->q_dot, 0, sim->stateVectorSize/2);
  memset(radau->q_ddot, 0, sim->stateVectorSize/2);
  memset(radau->p_dot, 0, sim->stateVectorSize/2);

  radau->q0 = (double*)malloc(sim->stateVectorSize/2);
  radau->p0 = (double*)malloc(sim->stateVectorSize/2);
  radau->q_dot0 = (double*)malloc(sim->stateVectorSize/2);
  radau->q_ddot0 = (double*)malloc(sim->stateVectorSize/2);
  radau->p_dot0 = (double*)malloc(sim->stateVectorSize/2);

  memset(radau->q0, 0, sim->stateVectorSize/2);
  memset(radau->p0, 0, sim->stateVectorSize/2);
  memset(radau->q_dot0, 0, sim->stateVectorSize/2);
  memset(radau->q_ddot0, 0, sim->stateVectorSize/2);
  memset(radau->p_dot0, 0, sim->stateVectorSize/2);

  memset(radau->dX, 0, sim->stateVectorSize);
  memset(radau->dXtemp, 0, sim->stateVectorSize);
  memset(radau->dX0, 0, sim->stateVectorSize);
  memset(radau->X, 0, sim->stateVectorSize);
  memset(radau->Xout, 0, sim->stateVectorSize);
  memset(radau->predictors, 0, sim->stateVectorSize);

  radau->Q = radau->X;
  radau->P = &radau->X[3*sim->n];
  radau->dQ = radau->dX;
  radau->dP = &radau->dX[3*sim->n];
  radau->Qout = radau->Xout;
  radau->Pout = &radau->Xout[sim->stateVectorLength/2];

// Copy to here so that we are ready to output to a file before we calculate osculating orbtis.
  memcpy(radau->Qout, sim->Q_dh, sim->stateVectorSize / 2);
  memcpy(radau->Pout, sim->P_dh, sim->stateVectorSize / 2);

  radau->rectifiedArray = (uint32_t*)malloc(sizeof(uint32_t)*sim->stateVectorLength);
  memset(radau->rectifiedArray, 0, sizeof(uint32_t)*sim->stateVectorLength);

  radau->b6_store = (double*)malloc(sim->stateVectorSize);
  radau->Xsize = (double*)malloc(2*sim->controlVectorSize);

  memset(radau->b6_store, 0, sim->stateVectorSize);
  memset(radau->Xsize, 0, 2*sim->controlVectorSize);

  //@todo should be able to remove these, but test.
  radau->fCalls = 0;
  radau->rectifications = 0;
  radau->stepsTaken = 0;
  radau->convergenceIterations = 0;
  radau->nextOutputTime = 0.0;

  // Copy across our tolerance fields.
  radau->aTol = sim->aTol;
  radau->rTol = sim->rTol;

  sim->fixed_step_size = sim->rTol <= 0? 1 : 0;

  radau->outputFile = fopen(sim->outputFile, "w");
  
  // configure our output spacing scheme
  if(sim->output_spacing == linear_spacing)
  {
    sim->outputInterval = (sim->tEnd - sim->t0) / sim->output_samples;
  }
  else if(sim->output_spacing == log_spacing)
  {
    radau->t0_lim = sim->t0 == 0.0 ? 1 : sim->t0;
    radau->base = pow(sim->tEnd/radau->t0_lim, 1.0/(double)sim->output_samples);
    radau->output_samples_count = 0;
  }

  if(radau->outputFile == NULL)
  {
    printf("\nPlease ensure that an output file is specified.");
  }

  if(radau->outputFile == NULL)
  {
    printf("\nFile open failed.");
  }

  sim->hInitial = sim->hInitial < MIN_STEP_SIZE ? MIN_STEP_SIZE : sim->hInitial;

  Terminate_Init(sim, &radau->fTerminate, combined);

  RadauStep15_Init(z_sim);
}

void Radau_Free(void)
{
  // Output footer to the file detailing the intergation.
  fprintf(radau->outputFile, "%f %f %.0f %.5E %.5E %.5E %.16f %f %f %f %.5E %.5E %s %.5E %.0f",
  sim->t0, sim->tEnd, (double)sim->n, (double)sim->orbits, radau->aTol, radau->rTol,
  sim->period, sim->outputInterval, sim->hInitial,
  sim->rectisPerOrbit, sim->dQcutoff, sim->dPcutoff, 0, radau->cpuTimeUsed, (double)radau->stepsTaken);

  fclose(radau->outputFile);

  RadauStep15_Free();
  Terminate_Free(sim);
  free(radau->dX);
  free(radau->dX0);
  free(radau->X);
  free(radau->Xout);
  free(radau->predictors);
  free(radau->rectifiedArray);
  free(radau);
}

double Radau_CalculateStepSize(double h, double hLast, double t)
{
  double hTrial = 0.0;

  // Get the error estimate and orbit size estimate.
  double errMax = radau->ReturnStepError(h, t);
    
  if(isnormal(errMax))
  {
    hTrial = h*pow(radau->rTol / errMax, (1.0/7.0));
  }
  else
  {
    hTrial = 1.1*h;
    fflush(stdout);
  }

  // Impose a minimum step size.
  hTrial = hTrial < MIN_STEP_SIZE ? MIN_STEP_SIZE : hTrial;

  // Limit step size growth to 4x per step.
  hTrial = (hTrial > MAX_STEP_SIZE_GROWTH*h) ? h*MAX_STEP_SIZE_GROWTH : hTrial;

  return hTrial;
}


void ClearRectifiedBFields(controlVars * B, uint32_t * rectifiedArray)
{
  for(uint32_t i = 0; i < sim->stateVectorLength; i++)
  {
    if(rectifiedArray[i] > 0)
    {
      B->p0[i] = 0.0;
      B->p1[i] = 0.0;
      B->p2[i] = 0.0;
      B->p3[i] = 0.0;
      B->p4[i] = 0.0;
      B->p5[i] = 0.0;
      B->p6[i] = 0.0;
    }
  }
}

static void OutputToFile(double t, uint32_t rectified, uint32_t iterations, double h, uint32_t forceOutput)
{
  double dH = 0;
  // Ensure we only output when necessary or at the end of an integration
  if(t >= radau->nextOutputTime || fabs(t-sim->tEnd) < 1E-15 || forceOutput)
  {
    double H = 0.0;

    if(sim->output_spacing == linear_spacing)
    {
      radau->nextOutputTime += sim->outputInterval;
    }
    else if(sim->output_spacing == log_spacing)
    {
      radau->output_samples_count++;
      radau->nextOutputTime = radau->t0_lim*pow(radau->base, radau->output_samples_count);      
    }

    // We havent initialised the osculating orbits at this point after initialisation.
    if(t == sim->t0)
    {
      H = sim->fCalculateInvariant(sim->Q_dh, sim->P_dh);
      dH = 1E-16;
    }
    else
    {
      sim->fPerformSummation(radau->Qout, radau->Pout, radau->dQ, radau->dP, FINAL_STAGE_INDEX);
      H = sim->fCalculateInvariant(radau->Qout, radau->Pout);
      dH = fabs((H-sim->H0)/sim->H0);
    }

    fprintf(radau->outputFile, "%.16E %.16E %.16E %.0f %f %f %0.f %0.f",
            t, H, dH, (double)radau->fCalls, h, (double)radau->rectifications,
            (double)rectified, (double)iterations);


    for(uint32_t i = 0; i < sim->n; i++)
    {
      fprintf(radau->outputFile, " %.16E %.16E %.16E", radau->Qout[3*i], radau->Qout[3*i+1], radau->Qout[3*i+2]);
    }

    for(uint32_t i = 0; i < sim->n; i++)
    {
      fprintf(radau->outputFile, " %.16E %.16E %.16E", radau->Pout[3*i], radau->Pout[3*i+1], radau->Pout[3*i+2]);
    }

    for(uint32_t i = 0; i < sim->n; i++)
    {
      fprintf(radau->outputFile, " %.16E %.16E %.16E", radau->dQ[3*i], radau->dQ[3*i+1], radau->dQ[3*i+2]);
    }

    for(uint32_t i = 0; i < sim->n; i++)
    {
      fprintf(radau->outputFile, " %.16E %.16E %.16E", radau->dP[3*i], radau->dP[3*i+1], radau->dP[3*i+2]);
    }

    fprintf(radau->outputFile, " %.16E %.16E", radau->b6Max, radau->accMax);
    fprintf(radau->outputFile, "\n");
  }
}
