/*!
 * Copyright (c) 2015 by Contributors
 * \file fm.cc
 * \brief Factorization Machines
 *
 */

#include <dmlc/io.h>
#include <dmlc/data.h>
#include <dmlc/logging.h>
#include <random>
#include "./fm.h"

namespace dmlc {
namespace fm {
class FmObjFunction : public solver::IObjFunction<float> {
public:

  // variables
  int nthread;      // training threads
  float reg_L2;     // L2 regularization
  float reg_L2_V;   // L2 regularization for V
  float fm_random;  // fm_random
  FmModel model;    // model
  dmlc::RowBlockIter<unsigned> *dtrain;   // training data
  dmlc::RowBlockIter<unsigned> *dval;     // validation data
  solver::LBFGSSolver<float> lbfgs;       // solver

  // constructor
  FmObjFunction() {
    lbfgs.SetObjFunction(this);
    nthread = 1;
    reg_L2 = 0.0f;
    reg_L2_V = reg_L2;
    fm_random = 0.01f;
    model.weight = NULL;
    dtrain = NULL;
    dval = NULL;
    data = "NULL";
    val_data = "NULL";
    task = "train";
    model_in = "NULL";
    name_pred = "pred.txt";
    name_dump = "dump.txt";
    model_out = "final.model";
  }
  // destructor
  virtual ~FmObjFunction(void) {
    if (dtrain != NULL)
      delete dtrain;
    if (dval != NULL)
      delete dval;
  }

  // set parameters
  inline void SetParam(const char *name, const char *val) {
    model.param.SetParam(name, val);
    lbfgs.SetParam(name, val);
    if (!strcmp(name, "num_feature")) {
      char ndigit[30];
      sprintf(ndigit, "%lu", model.param.num_weight);
      lbfgs.SetParam("num_dim", ndigit);
    }
    if (!strcmp(name, "reg_L2")) reg_L2 = static_cast<float>(atof(val));
    if (!strcmp(name, "reg_L2_V")) reg_L2_V = static_cast<float>(atof(val));
    if (!strcmp(name, "fm_random")) fm_random = static_cast<float>(atof(val));
    if (!strcmp(name, "nthread")) nthread = atoi(val);
    if (!strcmp(name, "task")) task = val;
    if (!strcmp(name, "model_in")) model_in = val;
    if (!strcmp(name, "model_out")) model_out = val;
    if (!strcmp(name, "name_pred")) name_pred = val;
    if (!strcmp(name, "name_dump")) name_dump = val;
    if (!strcmp(name, "data")) data = val;
    if (!strcmp(name, "val_data")) val_data = val;
  }

  // run
  inline void Run(void) {
    // create dtrain based on data
    if (data != "NULL") {
      rabit::TrackerPrintf("[Run@fm.cc] data = %s\n", data.c_str());
      dtrain = dmlc::RowBlockIter<unsigned>::Create
        (data.c_str(),
         rabit::GetRank(),
         rabit::GetWorldSize(),
         "libsvm");
    }
    // lodel model if exists
    if (model_in != "NULL") {
      this->LoadModel(model_in.c_str());
    }
    // run based on task type
    if (task == "train") {
      // init validation data
      if (val_data != "NULL") {
        InitValidation();
      }
      lbfgs.Run();
      if (rabit::GetRank() == 0) {
        rabit::TrackerPrintf("[Run@fm.cc] save model_out: %s\n", model_out.c_str());
        this->SaveModelWeight(lbfgs.GetWeight(), 0);
      }
    } else if (task == "pred") {
      this->TaskPred();
    } else if (task == "dump"){
      this->TaskDump(); 
    } else {
      LOG(FATAL) << "[Run@fm.cc] unknown task: " << task;
    }
  }
  inline void TaskPred(void) {
    CHECK(model_in != "NULL") << "must set model_in for task=pred";
    dmlc::Stream *fo = dmlc::Stream::Create(name_pred.c_str(), "w");
    dmlc::ostream os(fo);
    dtrain->BeforeFirst();
    while (dtrain->Next()) {
      const RowBlock<unsigned> &batch = dtrain->Value();
      for (size_t i = 0; i < batch.size; ++i) {
        os << model.Predict(batch[i]) << '\n';
      }
    }
    // make sure os push to the output stream, before delete
    os.set_stream(NULL);
    delete fo;
    rabit::TrackerPrintf("[TaskPred@fm.cc] finish writing to: %s\n", name_pred.c_str());
  }
  inline void TaskDump(void) {
    CHECK(model_in != "NULL") << "must set model_in for task=dump";
    dmlc::Stream *fo = dmlc::Stream::Create(name_dump.c_str(), "w");
    dmlc::ostream os(fo);
    for (size_t i = 0; i < model.param.num_feature; ++i) {
      os << i ; //<< '\t' << model.weight[i];
      for (int n = 0; n < model.param.nfactor; ++n) {
        os << '\t' << model.weight[i * model.param.nfactor + n];
      }
      os << '\n';
    }
    // os << "bias" << '\t' << model.weight[model.param.num_weight - 1] << '\n';
    os.set_stream(NULL);
    delete fo;
    rabit::TrackerPrintf("[TaskDump@fm.cc] finish dumping to %s\n", name_dump.c_str());
  }

  // init validation data
  inline void InitValidation(void){
    dval = dmlc::RowBlockIter<unsigned>::Create
      (val_data.c_str(),
       rabit::GetRank(),
       rabit::GetWorldSize(),
       "libsvm");
    lbfgs.SetValidation(true);
  }

  // load model
  inline void LoadModel(const char *fname) {
    Stream *fi = Stream::Create(fname, "r");
    std::string header; header.resize(4);
    // check header for different binary encode
    CHECK(fi->Read(&header[0], 4) != 0) << "invalid model";
    // base64 format
    if (header == "binf") {
      model.Load(fi);
    } else {
      LOG(FATAL) << "invalid model file";
    }
    delete fi;
  }

  // save model
  inline void SaveModel(const char *fname,
                        const float *wptr,
                        bool save_base64 = false) {
    Stream *fo = Stream::Create(fname, "w");
    fo->Write("binf", 4);
    model.Save(fo, wptr);
    delete fo;
  }

  // save model weight as readable
  virtual void SaveModelWeight(const float *wptr, size_t num_iteration) {
    std::ostringstream version;
    version << num_iteration;
    std::string modelversion = num_iteration == 0 ? this->model_out : this->model_out + "_V" + version.str();
    rabit::TrackerPrintf("[SaveModelWeight@fm.cc]: save model: %s\n", modelversion.c_str());
    dmlc::Stream *fo = dmlc::Stream::Create(modelversion.c_str(), "w");
    dmlc::ostream os(fo);
    size_t size = model.param.num_weight;
    for(size_t i = 0;i < size; ++i) {
      os << i << "\t" << *wptr << "\n";
      wptr++;
    }	
    os.set_stream(NULL);
    delete fo;
  }

  virtual void InitNumDim(size_t &dim, size_t &size)  {
    if (model_in == "NULL") {
      size_t ndim = dtrain->NumCol();
      size_t nsize = (dtrain->Value()).size;
      rabit::TrackerPrintf("[InitNumDim@fm.cc] @node[%d] train sample num: %d\n", rabit::GetRank(), nsize);
      // ndim is the max #column of each reducer
      rabit::Allreduce<rabit::op::Max>(&ndim, 1);
      // nsize is the sum of samples of each reducer
      rabit::Allreduce<rabit::op::Sum>(&nsize, 1);
      model.param.num_feature = std::max(ndim, model.param.num_feature);
      model.param.num_size = nsize;
      rabit::TrackerPrintf("[InitNumDim@fm.cc] single feature num max: %d\n", model.param.num_feature);
      rabit::TrackerPrintf("[InitNumDim@fm.cc] train sample num total: %ld\n", model.param.num_size);
      if (val_data != "NULL") {
        // no need of ndim_val: equal to ndim, ignore other columns
        size_t nsize_val = (dval->Value()).size;
        rabit::Allreduce<rabit::op::Sum>(&nsize_val, 1);
        model.param.num_size_val = nsize_val;
        rabit::TrackerPrintf("[InitNumDim@fm.cc] validation sample num total: %ld\n", model.param.num_size_val);
      }
    }
    // total #weights: #feature * nfactor + #bias
    dim = model.param.num_weight = model.param.num_feature * (model.param.nfactor);
    size = model.param.num_size;
  }

  // init model
  virtual void InitModel(float *weight, size_t size) {
    if (model_in == "NULL") {
      if(rabit::GetRank() == 0){
        // init weight with normal distribution (0,1) * fm_random
        std::default_random_engine generator;
        std::normal_distribution<float> distribution(0.0,1.0);
        for(size_t i = 0; i < size; ++i) {
          weight[i] = distribution(generator) * fm_random;
        }
      }
      //memset(weight, 0.0f, size * sizeof(float));
      model.param.InitBaseScore();
    } else {
      // set 0 if model_in exists
      rabit::Broadcast(model.weight, size * sizeof(float), 0);
      memcpy(weight, model.weight, size * sizeof(float));
    }
  }

  // load model
  virtual void Load(rabit::Stream *fi) {
    fi->Read(&model.param, sizeof(model.param));
  }
  // save model
  virtual void Save(rabit::Stream *fo) const {
    fo->Write(&model.param, sizeof(model.param));
  }

  // evaluation loss
  virtual double Eval(const float *weight, size_t size, bool validation) {
    if (nthread != 0) omp_set_num_threads(nthread);
    CHECK(size == model.param.num_weight);
    double sum_val = 0.0;

    if(validation){
      dval->BeforeFirst();
      while (dval->Next()) {
        const RowBlock<unsigned> &batch = dval->Value();
        #pragma omp parallel for schedule(static) reduction(+:sum_val)
        for (size_t i = 0; i < batch.size; ++i) {
          float py = model.param.PredictMargin(weight, batch[i]);
          float fv = model.param.MarginToLoss(batch[i].label, py) * batch[i].weight;
          sum_val += fv;
        }
      }
      CHECK(!std::isnan(sum_val)) << "nan occurs";
      return sum_val/model.param.num_size_val;
    }

    dtrain->BeforeFirst();
    while (dtrain->Next()) {
      const RowBlock<unsigned> &batch = dtrain->Value();
      #pragma omp parallel for schedule(static) reduction(+:sum_val)
      for (size_t i = 0; i < batch.size; ++i) {
        float py = model.param.PredictMargin(weight, batch[i]);
        // calculate (weighted) loss
        float fv = model.param.MarginToLoss(batch[i].label, py) * batch[i].weight;
        sum_val += fv;
      }
    }

    // calculate regularization loss
    if (rabit::GetRank() == 0) {
      // only add L2 regularization once
      //if (reg_L2 != 0.0f) {
      //  double sum_sqr = 0.0;
      //  for (size_t i = 0; i < model.param.num_feature; ++i) {
      //     sum_sqr += weight[i] * weight[i];
      //  }
      //  sum_val += 0.5 * reg_L2 * sum_sqr;        
      //}
      if (reg_L2_V != 0.0f) {
        double sum_sqr = 0.0;
        for (size_t i = 0; i < model.param.num_weight; ++i) {
          sum_sqr += weight[i] * weight[i];
        }
        sum_val += 0.5 * reg_L2_V * sum_sqr;
      }
    }
    CHECK(!std::isnan(sum_val)) << "nan occurs";
    return sum_val;
  }

  // calculate gradient
  virtual void CalcGrad(float *out_grad,
                        const float *weight,
                        size_t size) {
    if (nthread != 0) omp_set_num_threads(nthread);
    CHECK(size == model.param.num_weight) << "size consistency check";
    memset(out_grad, 0.0f, sizeof(float) * size);
    // double sum_gbias = 0.0;

    dtrain->BeforeFirst();    
    while (dtrain->Next()) {
      const RowBlock<unsigned> &batch = dtrain->Value();
      static std:: vector<float> tmp(size,0);
      static std:: vector<std::vector<float> > tmp_out_grad(nthread,tmp);
      #pragma omp parallel for schedule(static)
      for (size_t i_b = 0; i_b < batch.size; ++i_b) {
        Row<unsigned> v = batch[i_b];
        int thread_id = omp_get_thread_num();
        float py = model.param.Predict(weight, v);
        float grad = model.param.PredToGrad(v.label, py) * v.weight;
        //add bias
        // sum_gbias += grad;
        // single feature grad (w)
        //for (index_t j = 0; j < v.length; ++j) {
        //  tmp_out_grad[thread_id][v.index[j]] += v.get_value(j) * grad;
        //}
        // interation features grad (v)
        for(int i = 0; i < model.param.nfactor; ++i) {
          double sumxf = 0.0;
          for(index_t j = 0; j < v.length; ++j){
            int n = v.index[j] * model.param.nfactor + i;
            sumxf += weight[n] * v.get_value(j);
          }
          for(index_t j = 0; j < v.length; ++j) {
            int n = v.index[j] * model.param.nfactor + i;
            tmp_out_grad[thread_id][n] += v.get_value(j) * (sumxf - weight[n] * v.get_value(j)) * grad;
          }
        }
      }
      for(int i = 0; i < nthread;i++){
        #pragma omp parallel for 
        for(size_t j = 0; j < size;j++){
            out_grad[j] += tmp_out_grad[i][j];
            tmp_out_grad[i][j] = 0.0;
        }
      }
    }
    // out_grad[model.param.num_weight -1] = static_cast<float>(sum_gbias);
    if (rabit::GetRank() == 0) {
      // only add L2 regularization once
      // if (reg_L2 != 0.0f) {
      //   for (size_t i = 0; i < model.param.num_feature; ++i) {
      //     out_grad[i] += reg_L2 * weight[i];
      //   }
      // }
      if (reg_L2_V != 0.0f) {
        for (size_t i = 0; i < model.param.num_weight; ++i) {
          out_grad[i] += reg_L2_V * weight[i];
        }
      }
    }
  }
    
private:
  std::string task;
  std::string model_in;
  std::string model_out;
  std::string name_pred;
  std::string name_dump;
  std::string data;
  std::string val_data;
};

}  // namespace fm
}  // namespace dmlc

int main(int argc, char *argv[]) {
  if (argc < 1) {
    // intialize rabit engine
    rabit::Init(argc, argv);
    if (rabit::GetRank() == 0) {
      rabit::TrackerPrintf("Usage: param=val\n");
    }
    rabit::Finalize();
    return 0;
  }
  rabit::Init(argc, argv);
  dmlc::fm::FmObjFunction *fm = new dmlc::fm::FmObjFunction();
  rabit::TrackerPrintf("[main@fm.cc] setting up parameters @ Rank %d..\n", rabit::GetRank());
  for (int i = 1; i < argc; ++i) {
    char name[256], val[256];
    if (sscanf(argv[i], "%[^=]=%s", name, val) == 2) {
      fm->SetParam(name, val);
    }
  }
  rabit::TrackerPrintf("[main@fm.cc] ready to run model @ Rank %d..\n", rabit::GetRank());
  fm->Run();
  delete fm;
  rabit::Finalize();
  return 0;
}
