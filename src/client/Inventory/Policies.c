/* This file is part of clProbDist.
 *
 * Copyright 2015-2016  Pierre L'Ecuyer, Universite de Montreal and Advanced Micro Devices, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * Authors:
 *
 *   Nabil Kemerchou <kemerchn@iro.umontreal.ca>    (2015)
 *
 */

#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <string.h>
#include <clRNG/clRNG.h>
#include "../common.h"
#include "Policies.h"
#include "SimulateRuns.h"

/*! [clRNG header] */
#include <clRNG/mrg31k3p.h>
/*! [clRNG header] */

#include "../../include/clProbDist/gamma.h"
#include "../../include/clProbDist/normal.h"

//************************************************************************
// Policies
//************************************************************************
int one_Policy(cl_context context, cl_device_id device, cl_command_queue queue, void* data_)
{
	OnePolicyData* data = (OnePolicyData*)data_;
	int n = data->n;
	int n1 = data->n1;
	int m = data->m;
	int s = data->s;
	int S = data->S;
	int selectedDist = data->selectedDist;
	double lambda = data->lambda;
	double mu = data->mu;
	double sigma = data->sigma;
	simResult * results = data->SimResults;
	ExecType execType = data->execType;

	//Declare streams & vars 
	clrngMrg32k3aStream* stream_demand = NULL, *stream_order = NULL;
	clrngMrg32k3aStream *substreams_demand = NULL, *substreams_order = NULL;

	clprobdistGamma* gammaDist = NULL;

	clrngStatus err;
	clprobdistStatus err2;
	size_t streamBufferSize;
	size_t NbrStreams = ((execType == basic || execType == Case_a) ? 1 : n);

	//Create profit stat
	double *stat_profit = (double *)malloc(n * sizeof(double));

	//Creator used to reset the state of the base seed in case there is successive calls to the same "Option"
	clrngMrg32k3aStreamCreator* Creator = clrngMrg32k3aCopyStreamCreator(NULL, &err);
	check_error(err, "%s(): cannot create stream creator", __func__);

	//Create stream demand
	if (execType == basic || execType == Case_a || execType == Case_b) {
		stream_demand = clrngMrg32k3aCreateStreams(Creator, NbrStreams, &streamBufferSize, &err);
		check_error(err, "%s(): cannot create random stream demand", __func__);
	}

	//Set Poisson distribution Lambda's
	size_t poissonBufSize = 0;
	clprobdistPoisson* poissonDist = NULL;
	double* lambdaArr = NULL;

	if (selectedDist == 1){
		poissonDist = clprobdistPoissonCreate(lambda, &poissonBufSize, &err2);
		check_error(err2, "%s(): cannot create Poisson distribution", __func__);
	}
	else if (selectedDist == 2){
		clprobdistStatus err;

		gammaDist = clprobdistGammaCreate(9, 0.5, 15, NULL, &err); /*Alpha*/ /*Lambda*//*Decprec*/
		clrngMrg32k3aStream* stream = clrngMrg32k3aCreateStreams(NULL, 1, NULL, NULL);
		lambdaArr = (double*)malloc(m*sizeof(double));

		for (size_t i = 0; i < m; i++){
			double u = clrngMrg32k3aRandomU01(stream);
			lambdaArr[i] = clprobdistGammaInverseCDFWithObject(gammaDist, u, &err);
		}
	}

	//Set Noraml Distribution
	size_t normalBufSize = 0;
	clprobdistNormal* normalDist = clprobdistNormalCreate(mu, sigma, &normalBufSize, &err2);

	//*************************
	//Simulate on CPU
	if (execType == basic)
	{
		// in the document, this corresponds to the call to
		// inventorySimulateRunsOneStream()
		inventorySimulateRunsCPU(m, s, S, n, 
			                     lambdaArr, stream_demand, NULL, 
			                     selectedDist, poissonDist, normalDist, 
								 stat_profit, execType, results);

	}
	else if (execType == Case_a || execType == Case_b)
	{
		//Set the result object ExecOption
		if (results != NULL){
			if (execType == Case_a) results->ExecOption = 2;
			else results->ExecOption = 3;
		}

		stream_order = clrngMrg32k3aCreateStreams(Creator, NbrStreams, &streamBufferSize, &err);
		check_error(err, "%s(): cannot create random stream order", __func__);

		// in the document, this corresponds to the calls to
		// inventorySimulateRunsManyStreams()
		// inventorySimulateRunsSubstreams()
		inventorySimulateRunsCPU(m, s, S, n, 
			                    lambdaArr, stream_demand, stream_order, 
			                    selectedDist, poissonDist, normalDist, 
								stat_profit, execType, results);
	}

	//*************************
	//Simulate on Device
	if (execType != basic){

		if (stream_demand != NULL)
			clrngMrg32k3aRewindStreams(NbrStreams, stream_demand);
		if (stream_order != NULL)
			clrngMrg32k3aRewindStreams(NbrStreams, stream_order);

		if (execType == Case_a)
		{
			if (results != NULL) (&results[3])->ExecOption = 2;
			printf("\n+++++++++     On Device (case a) : One policy, two streams with their substreams \n");
			substreams_demand = clrngMrg32k3aMakeSubstreams(stream_demand, n, &streamBufferSize, &err);
			check_error(err, "%s(): cannot create random substreams demand", __func__);

			substreams_order = clrngMrg32k3aMakeSubstreams(stream_order, n, &streamBufferSize, &err);
			check_error(err, "%s(): cannot create random substreams order", __func__);

			inventorySimulateRunsGPU(context, device, queue, m, &s, &S, 1, n, 0, 0, OnePolicy, "inventorySimulateGPU", 
				                    streamBufferSize, substreams_demand, substreams_order,
									selectedDist, lambdaArr, 
									poissonBufSize, poissonDist, normalBufSize, normalDist,
									stat_profit, (results != NULL ? &results[3] : NULL));

		}
		else if (execType == Case_b)
		{
			if (results != NULL) (&results[3])->ExecOption = 3;
			printf("\n+++++++++     On Device (case b) : One policy, two arrays of n streams each \n");

			inventorySimulateRunsGPU(context, device, queue, m, &s, &S, 1, n, 0, 0, OnePolicy, "inventorySimulateGPU",
				streamBufferSize, stream_demand, stream_order,
				selectedDist, lambdaArr, 
				poissonBufSize, poissonDist, normalBufSize, normalDist,
				stat_profit, (results != NULL ? &results[3] : NULL));
		}
		else if (execType == Case_c)
		{
			if (results != NULL) results->ExecOption = 4;
			printf("\n+++++++++     On Device (case c) : One policy, using 2*n1 streams with n2 substreams on each \n");
			stream_demand = clrngMrg32k3aCreateStreams(Creator, n1, &streamBufferSize, &err);
			check_error(err, "%s(): cannot create random stream demand", __func__);

			stream_order = clrngMrg32k3aCreateStreams(Creator, n1, &streamBufferSize, &err);
			check_error(err, "%s(): cannot create random stream order", __func__);

			inventorySimulateRunsGPU(context, device, queue, m, &s, &S, 1, n, n1, n / n1, OnePolicy, "inventorySimulSubstreamsGPU",
				streamBufferSize, stream_demand, stream_order,
				selectedDist, lambdaArr, 
				poissonBufSize, poissonDist, normalBufSize, normalDist,
				stat_profit, results);
		}
		else if (execType == Case_d)
		{
			if (results != NULL) results->ExecOption = 5;
			printf("\n+++++++++     On Device (case d) : One policy, using 2*n streams with 2*n2 streams per Work item \n");
			stream_demand = clrngMrg32k3aCreateStreams(Creator, n, &streamBufferSize, &err);
			check_error(err, "%s(): cannot create random stream demand", __func__);

			stream_order = clrngMrg32k3aCreateStreams(Creator, n, &streamBufferSize, &err);
			check_error(err, "%s(): cannot create random stream order", __func__);

			inventorySimulateRunsGPU(context, device, queue, m, &s, &S, 1, n, n1, n / n1, OnePolicy, "inventorySimul_DistinctStreams_GPU",
				streamBufferSize, stream_demand, stream_order,
				selectedDist, lambdaArr, 
				poissonBufSize, poissonDist, normalBufSize, normalDist,
				stat_profit, results);
		}
	}

	//Free resources
	free(stat_profit);
	free(lambdaArr);
	clrngMrg32k3aDestroyStreams(stream_demand);
	clrngMrg32k3aDestroyStreams(stream_order);
	clrngMrg32k3aDestroyStreams(substreams_demand);
	clrngMrg32k3aDestroyStreams(substreams_order);
	clrngMrg32k3aDestroyStreamCreator(Creator);
	clprobdistNormalDestroy(normalDist);
	clprobdistPoissonDestroy(poissonDist);
	clprobdistGammaDestroy(gammaDist);

	return EXIT_SUCCESS;
}

int several_Policies(cl_context context, cl_device_id device, cl_command_queue queue, void* data_)
{
	SeveralPoliciesData* data = (SeveralPoliciesData*)data_;
	int n = data->n;
	int n1 = data->n1;
	int m = data->m;
	int* s = data->s;
	int* S = data->S;
	int P = data->P;
	int selectedDist = data->selectedDist;
	double lambda = data->lambda;
	double mu = data->mu;
	double sigma = data->sigma;
	simResult * results = data->SimResults;
	ExecOption _optionType = data->optionType;

	//Declare streams & vars
	clrngMrg32k3aStream* streams_demand = NULL, *streams_order = NULL;
	double *stat_profit = NULL, *stat_diff = NULL;
	clprobdistGamma* gammaDist = NULL;

	clrngStatus err;
	clprobdistStatus err2;
	size_t streamBufferSize;

	//Allocate stat profit for  n*P runs
	stat_profit = (double *)malloc(n * P * sizeof(double));

	//Creator used to reset the state of the base seed in case there is successive calls to the same "Option"
	clrngMrg32k3aStreamCreator* Creator = clrngMrg32k3aCopyStreamCreator(NULL, &err);
	check_error(err, "%s(): cannot create stream creator", __func__);

	//Create n1 Streams for demand and order
	streams_demand = clrngMrg32k3aCreateStreams(Creator, n1, &streamBufferSize, &err);
	check_error(err, "%s(): cannot create random streams demand", __func__);

	streams_order = clrngMrg32k3aCreateStreams(Creator, n1, &streamBufferSize, &err);
	check_error(err, "%s(): cannot create random streams order", __func__);

	//Poisson distribution
	size_t poissonBufSize=0;
	clprobdistPoisson* poissonDist = NULL;
	double* lambdaArr = NULL;

	if (selectedDist == 1){
		poissonDist = clprobdistPoissonCreate(lambda, &poissonBufSize, &err2);
		check_error(err2, "%s(): cannot create Poisson distribution", __func__);
	}
	else if (selectedDist == 2){
		clprobdistStatus err;

		gammaDist = clprobdistGammaCreate(9, 0.5, 15, NULL, &err); /*Alpha*/ /*Lambda*//*Decprec*/
		clrngMrg32k3aStream* stream = clrngMrg32k3aCreateStreams(NULL, 1, NULL, NULL);
		lambdaArr = (double*)malloc(m*sizeof(double));

		for (size_t i = 0; i < m; i++){
			double u = clrngMrg32k3aRandomU01(stream);
			lambdaArr[i] = clprobdistGammaInverseCDFWithObject(gammaDist, u, &err);
		}
	}

	//Set Noraml Distribution
	size_t normalBufSize = 0;
	clprobdistNormal* normalDist = clprobdistNormalCreate(mu, sigma, &normalBufSize, &err2);

	//printf("\n++++++++++++++++++++++++++++++     On Device   +++++++++++++++++++++++++++++++\n");
	if (_optionType == Option1)
	{
		printf("+++++++++     Simulate n2 runs on n1 work items using 2*n1 streams and n2 substreams for each, for P policies in series \n");
		printf("+++++++++     CRN simulation : \n");
		for (int k = 0; k < P; k++) {
			inventorySimulateRunsGPU(context, device, queue, m, &s[k], &S[k], P, n, n1, n / n1, Option1, "inventorySimulSubstreamsGPU",
				streamBufferSize, streams_demand, streams_order,
				selectedDist, lambdaArr, 
				poissonBufSize, poissonDist, normalBufSize, normalDist,
				&stat_profit[k*n], NULL);

			clrngMrg32k3aRewindStreams(n1, streams_demand);
			clrngMrg32k3aRewindStreams(n1, streams_order);
		}
		//Compute CI
		stat_diff = (double *)malloc(n * sizeof(double));
		for (int i = 0; i < n; i++)
			stat_diff[i] = stat_profit[n + i] - stat_profit[i];
		printf("\nDifference CRN :\n ------------\n");
		computeCI(n, stat_diff, NULL);

		printf("\n+++++++++     IRN simulation : \n");
		for (int k = 0; k < P; k++) {
			inventorySimulateRunsGPU(context, device, queue, m, &s[k], &S[k], P, n, n1, n / n1, Option1, "inventorySimulSubstreamsGPU",
				streamBufferSize, streams_demand, streams_order,
				selectedDist, lambdaArr, 
				poissonBufSize, poissonDist, normalBufSize, normalDist,
				&stat_profit[k*n], NULL);

			streams_demand = clrngMrg32k3aCreateStreams(NULL, n1, &streamBufferSize, &err);
			check_error(err, "%s(): cannot create random streams demand", __func__);

			streams_order = clrngMrg32k3aCreateStreams(NULL, n1, &streamBufferSize, &err);
			check_error(err, "%s(): cannot create random streams order", __func__);

		}
		//Compute CI
		for (int i = 0; i < n; i++)
			stat_diff[i] = stat_profit[n + i] - stat_profit[i];
		printf("\nDifference IRN:\n ------------\n");
		computeCI(n, stat_diff, NULL);
	}
	else { //Option2 

		if (results != NULL) results->ExecOption = 7;

		//+++++++++     Simulate n2 runs on n1p workitmes using n1 streams and n2 substreams, all P policies in parallel
		inventorySimulateRunsGPU(context, device, queue, m, s, S, P, n * P, n1 * P, n / n1, Option2, "inventorySimulPoliciesGPU",
			streamBufferSize, streams_demand, streams_order,
			selectedDist, lambdaArr, 
			poissonBufSize, poissonDist, normalBufSize, normalDist,
			stat_profit, results);

		//Compute CI
		stat_diff = (double *)malloc(n * sizeof(double));
		int n2 = n / n1;
		for (int j = 0; j < n2; j++)
			for (int i = 0; i < n1; i++)
			{
				int index = i + (j*n1*P);
				stat_diff[i + j*n1] = stat_profit[n1 + index] - stat_profit[index];
			}
		//printf("\nDifference:\n ------------\n");
		computeCI(n, stat_diff, results);
	}

	//Free Resources
	free(lambdaArr);
	clrngMrg32k3aDestroyStreams(streams_demand);
	clrngMrg32k3aDestroyStreams(streams_order);
	clrngMrg32k3aDestroyStreamCreator(Creator);
	clprobdistNormalDestroy(normalDist);
	clprobdistPoissonDestroy(poissonDist);
	clprobdistGammaDestroy(gammaDist);
	free(stat_diff);
	free(stat_profit);

	return EXIT_SUCCESS;
}
