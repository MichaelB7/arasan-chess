// Copyright 2010-2015 by Jon Dart. All Rights Reserved.
#include "board.h"
#include "boardio.h"
#include "notation.h"
#include "legal.h"
#include "hash.h"
#include "globals.h"
#include "chessio.h"
#include "util.h"
#include "search.h"
#include "tune.h"
#include "spsa.h"
#include <iostream>
#include <fstream>
#include <iomanip>
#include <map>
extern "C" {
#include <math.h>
#include <ctype.h>
#include <unistd.h>
};

//#define CMAES

#include "cmaes.h"
using namespace libcmaes;

// define to run optimizer code on a simple test problem
#define TEST

static int iterations = 40;

// script to run matches
static const char * MATCH_PATH="/home/jdart/tools/match.py";

static bool terminated = false;

static string out_file_name="params.cpp";

static string x0_file_name="x0";

static int first_index = 0;
 
static int last_index = tune::NUM_TUNING_PARAMS-1;

static int games = 1000;

static int best = 0;

static double scale(const tune::TuneParam &t) 
{
   return double(t.current-t.min_value)/double(t.max_value-t.min_value);
}

static int unscale(double val, const tune::TuneParam &t)
{
   return round(double(t.max_value-t.min_value)*val+(double)t.min_value);
}

static int exec(const char* cmd) {
   
   cout << "executing " << cmd << endl;

   FILE* pipe = popen(cmd, "r");
   if (!pipe) {
      cerr << "perror failed" << endl;
      return -1;
   }
   char buffer[2048];
   int result = -1;
   while(!feof(pipe)) {
      if(fgets(buffer, 2048, pipe) != NULL) {
         cout << buffer << endl;
         if (strlen(buffer)>7 && strncmp(buffer,"rating=",7)==0) {
            result = atoi(buffer+7);
         }
      }
   }
   pclose(pipe);
   if (result > best) {
      if (x0_file_name.length()) {
         ofstream x0_out(x0_file_name,ios::out | ios::trunc);
         tune::writeX0(x0_out);
      } else {
         tune::writeX0(cout);
      }
      if (out_file_name.length()) {
         ofstream param_out(out_file_name,ios::out | ios::trunc);
         Scoring::Params::write(param_out);
         param_out << endl;
      } else {
         Scoring::Params::write(cout);
         cout << endl;
      }
   }
   
   return result;
}

static double computeLsqError() {

   stringstream s;
   s << MATCH_PATH;
   s << ' ';
/*
   s << "-best ";
   s << best;
   s << ' ';
*/
   s << games;
   for (int i = first_index; i <= last_index; i++) {
      s << ' ' << tune::tune_params[i].name << ' ';
      s << tune::tune_params[i].current;
   }
   s << '\0';
   string cmd = s.str();
   // Call external script to run the matches and return a rating.
   // Optimizer minimizes, so optimize 5000-rating.
   return double(5000-exec(cmd.c_str()));
}

#ifdef CMAES
static FitFunc evaluator = [](const double *x, const int dim) 
{
   int i;
   for (i = 0; i < dim; i++) 
   {
      tune::tune_params[i+first_index].current = unscale(x[i+first_index],tune::tune_params[i+first_index]);
   }
   tune::initParams();
   double err = computeLsqError();
   cout << "objective=" << err << endl;
   return err;
   
};

static FitFunc test_evaluator = [](const double *x, const int dim) 
{
   double sum = 0;
   for (int i = 0; i < dim; i++) {
      double factor = 0.8;
      if (i % 2 == 0) factor = 1.1;
      sum += factor*pow(x[i],2.0);
   }
   cout << "returning " << 800.0*sqrt(sum) << endl;
   return 800.0*sqrt(sum);
};

static ProgressFunc<CMAParameters<GenoPheno<pwqBoundStrategy>>,CMASolutions> progress = [](const CMAParameters<GenoPheno<pwqBoundStrategy>> &cmaparams, const CMASolutions &cmasols)
{
   
   std::cout << "best solution: " << cmasols << std::endl;
   vector <double> x0 = cmasols.best_candidate().get_x();
/*
   cout << "denormalized solution: " << endl;
   int i = 0;
        
   for (vector<double>::const_iterator it = x0.begin();
        it != x0.end();
        it++,i++) {
      cout << " " << tune_params[i].name << ": " << round(unscale(*it,i)) << endl;
   }
   cout << endl;
*/   
#ifndef TEST
   if (x0_file_name.length()) {
      ofstream x0_out(x0_file_name,ios::out | ios::trunc);
      tune::writeX0(x0_out);
   } else {
      tune::writeX0(cout);
   }
   if (out_file_name.length()) {
      ofstream param_out(out_file_name,ios::out | ios::trunc);
      Scoring::Params::write(param_out);
      param_out << endl;
   } else {
      Scoring::Params::write(cout);
      cout << endl;
   }
#endif
   return 0;
};
#endif

#ifdef TEST
static double evaluate(const vector<double> &x) 
{
   double sum = 0;
   for (int i = 0; i < x.size(); i++) {
      double factor = 0.8;
      if (i % 2 == 0) factor = 1.1;
      sum += factor*pow(x[i],2.0);
   }
   return 800.0*sqrt(sum);
}
#else

static double evaluate(const vector<double> &x)
{
   for (int i = 0; i < tune::NUM_TUNING_PARAMS; i++) 
   {
      tune::tune_params[i].current = round(unscale(x[i],tune::tune_params[i]));
   }
   tune::initParams();
   double err = computeLsqError();
   return err;
};
#endif

void update(double obj, const vector<double> &x) 
{
   cout << "objective=" << obj << endl;
};


static void usage() 
{
   cerr << "Usage: tuner -i <input objective file>" << endl;
   cerr << "-o <output parameter file> -x <output objective file>" << endl;
   cerr << "-f <first_parameter_name> -s <last_parameter_name>" << endl;
   cerr << "-n <iterations>" << endl;
}

int CDECL main(int argc, char **argv)
{
    Bitboard::init();
    initOptions(argv[0]);
    Attacks::init();
    Scoring::init();
    if (!initGlobals(argv[0], false)) {
        cleanupGlobals();
        exit(-1);
    }
    atexit(cleanupGlobals);
    delayedInit();
    options.search.hash_table_size = 0;

//    if (EGTBMenCount) {
//        cerr << "Initialized tablebases" << endl;
//    }
    options.book.book_enabled = options.log_enabled = 0;
    options.learning.position_learning = false;
#if defined(GAVIOTA_TBS) || defined(NALIMOV_TBS)
    options.search.use_tablebases = false;
#endif
    options.search.easy_plies = 0;

    string input_file;
    
   cout << "writing initial solution" << endl;
   tune::initParams();
   ofstream param_out(out_file_name,ios::out | ios::trunc);
   Scoring::Params::write(param_out);
   param_out << endl;
   ofstream x0_out(x0_file_name,ios::out | ios::trunc);
   tune::writeX0(x0_out);

    int arg = 1;
    string first_param, last_param;
    
    while (arg < argc && argv[arg][0] == '-') {
       if (strcmp(argv[arg],"-i")==0) {
          ++arg;
          input_file = argv[arg];
       }
       else if (strcmp(argv[arg],"-g")==0) {
          ++arg;
          games = atoi(argv[arg]);
       }
       else if (strcmp(argv[arg],"-o")==0) {
          ++arg;
          out_file_name = argv[arg];
       }
       else if (strcmp(argv[arg],"-f")==0) {
          ++arg;
          first_param = argv[arg];
       }
       else if (strcmp(argv[arg],"-s")==0) {
          ++arg;
          last_param = argv[arg];
       }
       else if (strcmp(argv[arg],"-x")==0) {
          ++arg;
          x0_file_name = argv[arg];
       }
       else if (strcmp(argv[arg],"-n")==0) {
          ++arg;
          iterations = atoi(argv[arg]);
       } else {
          cerr << "invalid option: " << argv[arg] << endl;
          usage();
          exit(-1);
       }
       ++arg;
    }

    if (first_param.length()) {
       first_index = tune::findParamByName(first_param);
       if (first_index == -1) {
          cerr << "Error: Parameter named " << first_param << " not found." << endl;
          exit(-1);
       }
    }
    if (last_param.length()) {
       last_index = tune::findParamByName(last_param);
       if (last_index == -1) {
          cerr << "Error: Parameter named " << last_param << " not found." << endl;
          exit(-1);
       }
    }
    
    int dim = last_index - first_index;
    if (dim<=0) {
       cerr << "Error: 2nd named parameter is before 1st!" << endl;
       exit(-1);
    }
    cout << "dimension = " << dim << endl;
    cout << "games per core per iteration = " << games << endl;
    
    vector<double> x0;
#ifdef CMAES
    double sigma = 0.05;
    // use variant with box bounds
    double lbounds[dim],
       ubounds[dim];
#ifdef TEST
    dim = 10;
    for (int i = 0; i < dim; i++) {
       x0.push_back(0.8);
       lbounds[i] = 0.0;
       ubounds[i] = 1.0;
    }
    GenoPheno<pwqBoundStrategy> gp(lbounds,ubounds,dim);
    CMAParameters<GenoPheno<pwqBoundStrategy>> cmaparams(dim,&x0.front(),sigma,-1,0,gp);
    cmaparams.set_algo(sepaBIPOP_CMAES);
    cmaparams.set_max_fevals(iterations);
    CMASolutions cmasols = cmaes<GenoPheno<pwqBoundStrategy>>(test_evaluator,cmaparams,progress);
#else
    // initialize & normalize
    for (int i = 0; i < dim; i++) {
       x0.push_back(scale(tune::tune_params[i+first_index]));
       lbounds[i] = 0.0;
       ubounds[i] = 1.0;
    }
    GenoPheno<pwqBoundStrategy> gp(lbounds,ubounds,dim);
    CMAParameters<GenoPheno<pwqBoundStrategy>> cmaparams(dim,&x0.front(),sigma,-1,0,gp);
    cmaparams.set_algo(sepaBIPOP_CMAES);
    cmaparams.set_max_iter(iterations);
    CMASolutions cmasols = cmaes<GenoPheno<pwqBoundStrategy>>(evaluator,cmaparams,progress);
#endif
#else
#ifdef TEST
    Spsa s(10);
    for (int i = 0; i < 10; i++) {
       x0.push_back(0.8);
    }
    s.optimize(x0,iterations,evaluate,update);
#else
    Spsa s(tune::NUM_TUNING_PARAMS);
    for (int i = 0; i < tune::NUM_TUNING_PARAMS; i++) {
       x0.push_back(scale(tune::tune_params[i]));
    }
    s.optimize(x0,iterations,evaluate,update);
#endif
#endif
    return 0;
}
