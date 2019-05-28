#include "pcAdapter.h"
#include "pcError.h"
#include "pcUpdateMesh.h"
#include "pcSmooth.h"
#include "pcWriteFiles.h"
#include <SimUtil.h>
#include <SimPartitionedMesh.h>
#include <SimDiscrete.h>
#include "SimMeshMove.h"
#include "SimMeshTools.h"
#include "apfSIM.h"
#include "gmi_sim.h"
#include <PCU.h>
#include <cassert>
#include <phastaChef.h>
#include <maStats.h>
#include <apfShape.h>

extern void MSA_setBLSnapping(pMSAdapt, int onoff);

namespace pc {

  apf::Field* convertField(apf::Mesh* m,
    const char* inFieldname,
    const char* outFieldname) {
    apf::Field* inf = m->findField(inFieldname);
    assert(inf);
    int size = apf::countComponents(inf);
    apf::Field* outf = m->findField(outFieldname);
    if (outf)
      apf::destroyField(outf);
    outf = apf::createPackedField(m, outFieldname, size);
    apf::NewArray<double> inVal(size);
    apf::NewArray<double> outVal(size);
    apf::MeshEntity* vtx;
    apf::MeshIterator* it = m->begin(0);
    while ((vtx = m->iterate(it))) {
      apf::getComponents(inf, vtx, 0, &inVal[0]);
      for (int i = 0; i < size; i++){
        outVal[i] = inVal[i];
      }
      apf::setComponents(outf,vtx, 0, &outVal[0]);
    }
    m->end(it);
    apf::destroyField(inf);
    return outf;
  }

  void meshSizeClamp(apf::Mesh2*& m, ph::Input& in, apf::Field* sizes) {
    assert(sizes);
    apf::Vector3 v_mag = apf::Vector3(0.0,0.0,0.0);
    apf::MeshEntity* v;
    apf::MeshIterator* vit = m->begin(0);
    while ((v = m->iterate(vit))) {
      apf::getVector(sizes,v,0,v_mag);
      for (int i = 0; i < 3; i++) {
        if(v_mag[i] < in.simSizeLowerBound) v_mag[i] = in.simSizeLowerBound;
        if(v_mag[i] > in.simSizeUpperBound) v_mag[i] = in.simSizeUpperBound;
      }
      apf::setVector(sizes,v,0,v_mag);
    }
    m->end(vit);
  }

  void attachMeshSizeField(apf::Mesh2*& m, ph::Input& in) {
    /* create a field to store mesh size */
    if(m->findField("sizes")) apf::destroyField(m->findField("sizes"));
    apf::Field* sizes = apf::createSIMFieldOn(m, "sizes", apf::VECTOR);
    phSolver::Input inp("solver.inp", "input.config");
    /* switch between VMS error mesh size and initial mesh size */
    if((string)inp.GetValue("Error Estimation Option") != "False") {
      pc::attachVMSSizeField(m, in, inp);
    }
    else {
      if(m->findField("frames")) apf::destroyField(m->findField("frames"));
      apf::Field* frames = apf::createSIMFieldOn(m, "frames", apf::MATRIX);
      ph::attachSIMSizeField(m, sizes, frames);
    }
    /* add mesh smooth/gradation function here */
    pc::addSmoother(m, in.gradingFactor);
    /* limit mesh size in a range */
    pc::meshSizeClamp(m, in, sizes);
  }

  int getNumOfMappedFields(phSolver::Input& inp) {
    /* initially, we have 7 fields: pressure, velocity, temperature,
       time der of pressure, time der of velocity, time der of temperature,
       ,mesh velocity and mesh size field */
    int numOfMappedFields = 8;
    return numOfMappedFields;
  }

  /* remove all fields except for solution, time
     derivative of solution, mesh velocity and keep
     certain field if corresponding option is on */
  void removeOtherFields(apf::Mesh2*& m, phSolver::Input& inp) {
    int index = 0;
    int numOfPackFields = 4;
    while (m->countFields() > numOfPackFields) {
      apf::Field* f = m->getField(index);
      if ( f == m->findField("solution") ||
           f == m->findField("time derivative of solution") ||
           f == m->findField("mesh_vel") ||
           f == m->findField("sizes") ) {
        index++;
        continue;
      }
      m->removeField(f);
      apf::destroyField(f);
    }
    m->verify();
  }

  int getSimFields(apf::Mesh2*& m, int simFlag, pField* sim_flds, phSolver::Input& inp) {
    int num_flds = 0;
    if (m->findField("solution")) {
      num_flds += 3;
      sim_flds[0] = apf::getSIMField(chef::extractField(m,"solution","pressure",1,apf::SCALAR,simFlag));
      sim_flds[1] = apf::getSIMField(chef::extractField(m,"solution","velocity",2,apf::VECTOR,simFlag));
      sim_flds[2] = apf::getSIMField(chef::extractField(m,"solution","temperature",5,apf::SCALAR,simFlag));
      apf::destroyField(m->findField("solution"));
    }

    if (m->findField("time derivative of solution")) {
      num_flds += 3;
      sim_flds[3] = apf::getSIMField(chef::extractField(m,"time derivative of solution","der_pressure",1,apf::SCALAR,simFlag));
      sim_flds[4] = apf::getSIMField(chef::extractField(m,"time derivative of solution","der_velocity",2,apf::VECTOR,simFlag));
      sim_flds[5] = apf::getSIMField(chef::extractField(m,"time derivative of solution","der_temperature",5,apf::SCALAR,simFlag));
      apf::destroyField(m->findField("time derivative of solution"));
    }

    if (m->findField("mesh_vel")) {
      num_flds += 1;
      sim_flds[6] = apf::getSIMField(chef::extractField(m,"mesh_vel","mesh_vel_sim",1,apf::VECTOR,simFlag));
      apf::destroyField(m->findField("mesh_vel"));
    }

    if (m->findField("sizes")) {
      num_flds += 1;
      sim_flds[7] = apf::getSIMField(chef::extractField(m,"sizes","sizes_sim",1,apf::VECTOR,simFlag));
      apf::destroyField(m->findField("sizes"));
    }

    return num_flds;
  }

  /* unpacked solution into serveral fields,
     put these field explicitly into pPList */
  pPList getSimFieldList(ph::Input& in, apf::Mesh2*& m){
    /* load input file for solver */
    phSolver::Input inp("solver.inp", "input.config");
    int num_flds = getNumOfMappedFields(inp);
    removeOtherFields(m,inp);
    pField* sim_flds = new pField[num_flds];
    getSimFields(m, in.simmetrixMesh, sim_flds, inp);
    pPList sim_fld_lst = PList_new();
    for (int i = 0; i < num_flds; i++) {
      PList_append(sim_fld_lst, sim_flds[i]);
    }
    assert(num_flds == PList_size(sim_fld_lst));
    delete [] sim_flds;
    return sim_fld_lst;
  }

  void measureIsoMeshAndWrite(apf::Mesh2*& m, ph::Input& in) {
    apf::Field* sizes = m->findField("sizes_sim");
    assert(sizes);
    apf::Field* isoSize = apf::createFieldOn(m, "iso_size", apf::SCALAR);
    apf::Vector3 v_mag = apf::Vector3(0.0,0.0,0.0);
    apf::MeshEntity* v;
    apf::MeshIterator* vit = m->begin(0);
    while ((v = m->iterate(vit))) {
      apf::getVector(sizes,v,0,v_mag);
      apf::setScalar(isoSize,v,0,v_mag[0]);
    }
    m->end(vit);

    if (PCU_Comm_Self() == 0)
      printf("\n evaluating the statistics! \n");
    // get the stats
    ma::SizeField* sf = ma::makeSizeField(m, isoSize);
    std::vector<double> el, lq;
    ma::stats(m, sf, el, lq, true);

    // create field for visualizaition
    apf::Field* f_lq = apf::createField(m, "linear_quality", apf::SCALAR, apf::getConstant(m->getDimension()));

    // attach cell-based mesh quality
    int n;
    apf::MeshEntity* r;
    if (m->getDimension() == 3)
      n = apf::countEntitiesOfType(m, apf::Mesh::TET);
    else
      n = apf::countEntitiesOfType(m, apf::Mesh::TRIANGLE);
    size_t i = 0;
    apf::MeshIterator* rit = m->begin(m->getDimension());
    while ((r = m->iterate(rit))) {
      if (! apf::isSimplex(m->getType(r))) {// ignore non-simplex elements
        apf::setScalar(f_lq, r, 0, 100.0); // set as 100
      }
      else {
        apf::setScalar(f_lq, r, 0, lq[i]);
        ++i;
      }
    }
    m->end(rit);
    PCU_ALWAYS_ASSERT(i == (size_t) n);

    // attach current mesh size
    if(m->findField("sizes")) apf::destroyField(m->findField("sizes"));
    if(m->findField("frames")) apf::destroyField(m->findField("frames"));
    apf::Field* aniSizes  = apf::createSIMFieldOn(m, "sizes", apf::VECTOR);
    apf::Field* aniFrames = apf::createSIMFieldOn(m, "frames", apf::MATRIX);
    ph::attachSIMSizeField(m, aniSizes, aniFrames);

    // write out mesh
    pc::writeSequence(m,in.timeStepNumber,"mesh_stats_");

    // delete fields
    apf::destroyField(sizes);
    apf::destroyField(aniSizes);
    apf::destroyField(aniFrames);
    apf::destroyField(isoSize);
    apf::destroyField(f_lq);
  }

  void transferSimFields(apf::Mesh2*& m) {
    if (m->findField("pressure")) // assume we had solution before
      chef::combineField(m,"solution","pressure","velocity","temperature");
    if (m->findField("der_pressure")) // assume we had time derivative of solution before
      chef::combineField(m,"time derivative of solution","der_pressure","der_velocity","der_temperature");
    if (m->findField("mesh_vel_sim"))
      convertField(m, "mesh_vel_sim", "mesh_vel");
    // destroy mesh size field
    if(m->findField("sizes"))  apf::destroyField(m->findField("sizes"));
    if(m->findField("frames")) apf::destroyField(m->findField("frames"));
  }

  void setupSimImprover(pVolumeMeshImprover vmi, pPList sim_fld_lst) {
    VolumeMeshImprover_setModifyBL(vmi, 1);
    VolumeMeshImprover_setShapeMetric(vmi, ShapeMetricType_VolLenRatio, 0.3);

    /* set fields to be mapped */
    if (PList_size(sim_fld_lst))
      VolumeMeshImprover_setMapFields(vmi, sim_fld_lst);
  }

  void setupSimAdapter(pMSAdapt adapter, ph::Input& in, apf::Mesh2*& m, pPList sim_fld_lst) {
    MSA_setAdaptBL(adapter, 1);
    MSA_setExposedBLBehavior(adapter,BL_DisallowExposed);
    MSA_setBLSnapping(adapter, 0); // currently needed for parametric model
    MSA_setBLMinLayerAspectRatio(adapter, 0.0); // needed in parallel

    /* use current size field */
    if(!PCU_Comm_Self())
      printf("Start mesh adapt of setting size field\n");

    apf::Field* sizes = m->findField("sizes_sim");
    assert(sizes);
    apf::Vector3 v_mag = apf::Vector3(0.0,0.0,0.0);
    apf::MeshEntity* v;
    apf::MeshIterator* vit = m->begin(0);
    while ((v = m->iterate(vit))) {
      apf::getVector(sizes,v,0,v_mag);
      pVertex meshVertex = reinterpret_cast<pVertex>(v);
      MSA_setVertexSize(adapter, meshVertex, v_mag[0]);
    }
    m->end(vit);

    /* set fields to be mapped */
    if (PList_size(sim_fld_lst))
      MSA_setMapFields(adapter, sim_fld_lst);
  }

  void runMeshAdapter(ph::Input& in, apf::Mesh2*& m, apf::Field*& orgSF, int step) {
    /* use the size field of the mesh before mesh motion */
    apf::Field* szFld = orgSF;

    if(in.simmetrixMesh == 1) {
      if (in.writeSimLog)
        Sim_logOn("sim_mesh_adaptation.log");
      pProgress progress = Progress_new();
      Progress_setDefaultCallback(progress);

      apf::MeshSIM* sim_m = dynamic_cast<apf::MeshSIM*>(m);
      pParMesh sim_pm = sim_m->getMesh();
      pMesh pm = PM_mesh(sim_pm,0);

      gmi_model* gmiModel = sim_m->getModel();
      pGModel model = gmi_export_sim(gmiModel);

      // declaration
      VIter vIter;
      pVertex meshVertex;

      /* attach mesh size field */
      attachMeshSizeField(m, in);

      /* set fields to be mapped */
      pPList sim_fld_lst = PList_new();
      PList_clear(sim_fld_lst);
      if (in.solutionMigration)
        sim_fld_lst = getSimFieldList(in, m);

      /* create the Simmetrix adapter */
      if(!PCU_Comm_Self())
        printf("Start mesh adapt\n");
      pMSAdapt adapter = MSA_new(sim_pm, 1);
      setupSimAdapter(adapter, in, m, sim_fld_lst);

      /* run the adapter */
      if(!PCU_Comm_Self())
        printf("do real mesh adapt\n");
      MSA_adapt(adapter, progress);
      MSA_delete(adapter);

      /* create Simmetrix improver */
      pVolumeMeshImprover vmi = VolumeMeshImprover_new(sim_pm);
      setupSimImprover(vmi, sim_fld_lst);

      /* run the improver */
      VolumeMeshImprover_execute(vmi, progress);
      VolumeMeshImprover_delete(vmi);

      PList_clear(sim_fld_lst);
      PList_delete(sim_fld_lst);

      /* load balance */
      pc::balanceEqualWeights(sim_pm, progress);

      /* write out mesh quality statistic info */
      if (in.measureAdaptedMesh)
        measureIsoMeshAndWrite(m, in);

      /* write mesh */
      if(!PCU_Comm_Self())
        printf("write mesh after mesh adaptation\n");
      writeSIMMesh(sim_pm, in.timeStepNumber, "sim_mesh_");
      Progress_delete(progress);

      /* transfer data back to apf */
      if (in.solutionMigration)
        transferSimFields(m);
    }
    else {
      assert(szFld);
      apf::synchronize(szFld);
      apf::synchronize(m->getCoordinateField());
      /* do SCOREC mesh adaptation */
      chef::adapt(m,szFld,in);
      chef::balance(in,m);
    }
    m->verify();
  }

}
