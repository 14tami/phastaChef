#include "chefPhasta.h"
#include <PCU.h>
#include <pcu_util.h>
#include <pcu_io.h>
#include <chef.h>
#include <phasta.h>
#include "phIO.h"
#include <ph.h>
#include <phBC.h>
#include <phstream.h>
#include <phastaChef.h>
#include <apfMDS.h>
#include <apfShape.h>
#include <apfDynamicVector.h>

#include <apfSIM.h>
#include <gmi_sim.h>
#include <SimPartitionedMesh.h>
#include <SimField.h>
#include "SimMeshTools.h"
#include <SimAdvMeshing.h>
#include "SimParasolidKrnl.h"
#include <SimDiscrete.h>
#include <MeshSimAdapt.h>

#include "pcAdapter.h"

#include <cstring>
#include <cassert>
#include <stdlib.h>
#include <unistd.h>

int integrationOrder = 1;

namespace {
  void freeMesh(apf::Mesh* m) {
    m->destroyNative();
    apf::destroyMesh(m);
  }

  /* may not need this */
  static FILE* openfile_read(ph::Input&, const char* path) {
    FILE* f = NULL;
    f = pcu_group_open(path, false);
    return f;
  }

  /* may not need this */
  static FILE* openstream_read(ph::Input& in, const char* path) {
    std::string fname(path);
    std::string restartStr("restart");
    FILE* f = NULL;
    if( fname.find(restartStr) != std::string::npos )
      f = openRStreamRead(in.rs);
    else {
      fprintf(stderr,
        "ERROR %s type of stream %s is unknown... exiting\n",
        __func__, fname.c_str());
      exit(1);
    }
    return f;
  }

  void setupChef(ph::Input& ctrl, int step) {
    PCU_ALWAYS_ASSERT(step > 0);
    //don't split or tetrahedronize
    ctrl.splitFactor = 1;
    ctrl.tetrahedronize = 0;
    ctrl.solutionMigration = 1;
    ctrl.adaptFlag = 0;
    ctrl.writeGeomBCFiles = 1;
  }

  /* remove all fields except for solution */
  void removeFieldsExceptSol(apf::Mesh2*& m) {
    int index = 0;
    int numOfPackFields = 1;
    while (m->countFields() > numOfPackFields) {
      apf::Field* f = m->getField(index);
      if ( f == m->findField("solution") ) {
        index++;
        continue;
      }
      m->removeField(f);
      apf::destroyField(f);
    }
    m->verify();
  }

  void calculateEfficiency(ph::Input src_ctrl, apf::Mesh2*& src_m,
                           ph::Input dst_ctrl, apf::Mesh2*& dst_m) {
    pProgress progress = Progress_new();
    Progress_setDefaultCallback(progress);

    // load fields from the source/reference mesh
    chef::readAndAttachFields(src_ctrl,src_m);
    PCU_ALWAYS_ASSERT(src_m->findField("solution"));
    removeFieldsExceptSol(src_m);
    int num_flds = 3;
    pField* src_sol_fld = new pField[num_flds];
    src_sol_fld[0] = apf::getSIMField(chef::extractField(src_m,"solution","pressure",1,apf::SCALAR,1));
    src_sol_fld[1] = apf::getSIMField(chef::extractField(src_m,"solution","velocity",2,apf::VECTOR,1));
    src_sol_fld[2] = apf::getSIMField(chef::extractField(src_m,"solution","temperature",5,apf::SCALAR,1));
    apf::destroyField(src_m->findField("solution"));

    // load fields from the destination mesh
    chef::readAndAttachFields(dst_ctrl,dst_m);
    apf::Field* dst_sol_fld = dst_m->findField("solution");
    apf::Field* dst_vms_fld = dst_m->findField("VMS_error");
    PCU_ALWAYS_ASSERT(dst_sol_fld);
    PCU_ALWAYS_ASSERT(dst_vms_fld);
    PCU_ALWAYS_ASSERT(apf::countComponents(dst_vms_fld) == 5);

    // create efficiency fields on destination mesh
    int nsd = dst_m->getDimension();
    apf::Field* p_eff_fld = apf::createField(dst_m,"pressure_efficiency",apf::SCALAR,apf::getConstant(nsd));
    apf::Field* v_eff_fld = apf::createField(dst_m,"velocity_efficiency",apf::VECTOR,apf::getConstant(nsd));
    apf::Field* t_eff_fld = apf::createField(dst_m,"temperature_efficiency",apf::SCALAR,apf::getConstant(nsd));
    PCU_Barrier();

    // load src model and mesh
    apf::MeshSIM* src_apf_msim = dynamic_cast<apf::MeshSIM*>(src_m);
    pParMesh src_ppm = src_apf_msim->getMesh();
    gmi_model* src_gmiModel = src_apf_msim->getModel();
    pGModel src_model = gmi_export_sim(src_gmiModel);

    pGRegion modelRegion;
    GRIter grIter;
    pGEntMeshMigrator gmig;
    // migrate all elements to rank 0
    gmig = GEntMeshMigrator_new(src_ppm, 3);
    grIter = GM_regionIter(src_model);
    while((modelRegion = GRIter_next(grIter))){
      int gid = PMU_gid(0, 0);
      GEntMeshMigrator_add(gmig, modelRegion, gid);
    }
    GRIter_delete(grIter);
    GEntMeshMigrator_run(gmig, progress);
    GEntMeshMigrator_delete(gmig);
    pMesh src_mesh = PM_mesh(src_ppm,0);

    // load dest model and mesh
    apf::MeshSIM* dst_apf_msim = dynamic_cast<apf::MeshSIM*>(dst_m);
    pParMesh dst_ppm = dst_apf_msim->getMesh();
    gmi_model* dst_gmiModel = dst_apf_msim->getModel();
    pGModel dst_model = gmi_export_sim(dst_gmiModel);

    // migrate all elements to rank 0
    gmig = GEntMeshMigrator_new(dst_ppm, 3);
    grIter = GM_regionIter(dst_model);
    while((modelRegion = GRIter_next(grIter))){
      int gid = PMU_gid(0, 0);
      GEntMeshMigrator_add(gmig, modelRegion, gid);
    }
    GRIter_delete(grIter);
    GEntMeshMigrator_run(gmig, progress);
    GEntMeshMigrator_delete(gmig);
    pMesh dst_mesh = PM_mesh(dst_ppm,0);

    // delcaration
    double searchFactor = 0.5; // need to be as user's input
    pGRegion src_modelRegion;
    pGRegion dst_modelRegion;
    pRegion dst_meshRegion;
    pEdge dst_meshEdge;
    pVertex dst_meshVertex;
    pPList deList = PList_new();
    apf::MeshEntity* dst_r;
    apf::MeshElement* dst_elm;
    apf::Element* dst_fd_elm;
    apf::Vector3 qpt;
    apf::Vector3 xyz;
    apf::Matrix3x3 J;
    double weight;
    double Jdet;
    double p_err_total = 0.0;
    double p_vms_total = 0.0;
    double v_err_total[3] = {0.0, 0.0, 0.0};
    double v_vms_total[3] = {0.0, 0.0, 0.0};
    double t_err_total = 0.0;
    double t_vms_total = 0.0;

    // loop over model regions
    grIter = GM_regionIter(src_model);
    while((src_modelRegion = GRIter_next(grIter))){
    // add model region to domain
      pGDomain gdom = GDomain_new();
      GDomain_addModelEntity(gdom, src_modelRegion, 1);

    // start mesh region finder
      pMeshRegionFinder mrf = MeshRegionFinder_new(src_mesh, 1.0, gdom, progress);

    // we assume that the "same" model region has the same tag
      dst_modelRegion = (pGRegion) GM_entityByTag(dst_model, 3, GEN_tag(src_modelRegion));

    // loop over destination mesh regions
      RIter rIter = M_classifiedRegionIter(dst_mesh, dst_modelRegion);
      while((dst_meshRegion = RIter_next(rIter))){
    // calculate shortest edge in this element
        int useFirFlag = 1;
        double min = 0.0;
        deList = R_edges(dst_meshRegion, 1);
        void *eiter = 0;
        while((dst_meshEdge = (pEdge)PList_next(deList, &eiter))){
          double el = E_length(dst_meshEdge);
          if (useFirFlag) {
            min = el;
            useFirFlag = 0;
          }
          else {
            if (el < min)
              min = el;
          }
        }
        PList_clear(deList);
        double tol = searchFactor * min;

    // loop over quadrature points
        dst_r = reinterpret_cast<apf::MeshEntity*>(dst_meshRegion);
        dst_elm = apf::createMeshElement(dst_m,dst_r);
        int numqpt = apf::countIntPoints(dst_elm,integrationOrder);
        double p_err_elm = 0.0;
        apf::Vector3 v_err_elm = apf::Vector3(0.0, 0.0, 0.0);
        double t_err_elm = 0.0;
        apf::Element* dst_sol_fd_elm = apf::createElement(dst_sol_fld,dst_elm);
        for(int i=0;i<numqpt;i++){
          apf::getIntPoint(dst_elm,integrationOrder,i,qpt);
          weight = apf::getIntWeight(dst_elm,integrationOrder,i);
          apf::getJacobian(dst_elm,qpt,J);
          J = apf::transpose(J); //Unique to PUMI implementation
          if(nsd==2) J[2][2] = 1.0;
          Jdet = fabs(apf::getJacobianDeterminant(J,nsd));
          apf::mapLocalToGlobal(dst_elm,qpt,xyz);
    // find this location in reference mesh
          const double loc[3] = {xyz[0], xyz[1], xyz[2]};
          double params[3] = {0.0, 0.0, 0.0};
          double dist[1] = {0.0};
          pRegion foundMR = MeshRegionFinder_find(mrf, loc, params, dist);
          if(foundMR && (dist[0] < tol)) {
    // get value from destination mesh
            apf::DynamicVector dst_sol(apf::countComponents(dst_sol_fld));
            apf::getComponents(dst_sol_fd_elm,qpt,&dst_sol[0]);
    // get value from source mesh
            double* src_sol_scl = new double[1];
            double* src_sol_vec = new double[3];
            pInterp intp0 = Field_entInterp(src_sol_fld[0], foundMR);
            pInterp intp1 = Field_entInterp(src_sol_fld[1], foundMR);
            pInterp intp2 = Field_entInterp(src_sol_fld[2], foundMR);
    // calculate pressure true error
            Interp_deriv0(intp0, params, 0, src_sol_scl);
            p_err_elm += (dst_sol[0] - src_sol_scl[0])*(dst_sol[0] - src_sol_scl[0])*weight*Jdet;
    // calculate velocity true error
            Interp_deriv0(intp1, params, 0, src_sol_vec);
            v_err_elm[0] += (dst_sol[1] - src_sol_vec[0])*(dst_sol[1] - src_sol_vec[0])*weight*Jdet;
            v_err_elm[1] += (dst_sol[2] - src_sol_vec[1])*(dst_sol[2] - src_sol_vec[1])*weight*Jdet;
            v_err_elm[2] += (dst_sol[3] - src_sol_vec[2])*(dst_sol[3] - src_sol_vec[2])*weight*Jdet;
    // calculate temperature true error
            Interp_deriv0(intp2, params, 0, src_sol_scl);
            t_err_elm += (dst_sol[4] - src_sol_scl[0])*(dst_sol[4] - src_sol_scl[0])*weight*Jdet;
/*
            apf::Matrix3x3 velGrad;
            apf::Matrix3x3 velRefGrad;
            apf::getVectorGrad(velElem,qpt,velGrad);
            apf::getVectorGrad(velRefElem,qpt,velRefGrad);

            apf::Matrix3x3 errGrad = velGrad-velRefGrad;
            for(int j=0; j< 3; j++){
              for(int k=0;k<3; k++){
                tempH1semi+=errGrad[j][k]*errGrad[j][k]*weight*Jdet;
              }
            }
*/
            Interp_delete(intp0);
            Interp_delete(intp1);
            Interp_delete(intp2);
          }
          else {
            if (foundMR == 0)
              printf("cannot find mesh region by point (%f,%f,%f)\n",loc[0],loc[1],loc[2]);
            else
              printf("dist %f is larger than tol %f\n",dist[0],tol);
          } // end if found mesh region
        } // end loop over quadrature points
    // calculate local efficiency and store in a field
        apf::NewArray<double> vms_elm(apf::countComponents(dst_vms_fld));
        apf::getComponents(dst_vms_fld,dst_r,0,&vms_elm[0]);
        apf::setScalar(p_eff_fld,dst_r,0,vms_elm[0]/sqrt(p_err_elm));
        apf::Vector3 v_eff_elm = apf::Vector3(vms_elm[1]/sqrt(v_err_elm[0]),
                                              vms_elm[2]/sqrt(v_err_elm[1]),
                                              vms_elm[3]/sqrt(v_err_elm[2]));
        apf::setScalar(t_eff_fld,dst_r,0,vms_elm[4]/sqrt(t_err_elm));
    // record global variables
        if (xyz[1] <= -0.44) { // hardcoding!!!!! only for plane layer BL case
          p_err_total    += sqrt(p_err_elm);
          p_vms_total    += vms_elm[0];
          v_err_total[0] += sqrt(v_err_elm[0]);
          v_vms_total[0] += vms_elm[1];
          v_err_total[1] += sqrt(v_err_elm[1]);
          v_vms_total[1] += vms_elm[2];
          v_err_total[2] += sqrt(v_err_elm[2]);
          v_vms_total[2] += vms_elm[3];
          t_err_total    += sqrt(t_err_elm);
          t_vms_total    += vms_elm[4];
        }
      } // end loop over mesh regions
      RIter_delete(rIter);
      MeshRegionFinder_delete(mrf);
      GDomain_delete(gdom);
    } // end loop over model regions
    GRIter_delete(grIter);

    // communicate global variables
    PCU_Add_Doubles(&p_err_total,1);
    PCU_Add_Doubles(&p_vms_total,1);
    PCU_Add_Doubles(&(v_err_total[0]),3);
    PCU_Add_Doubles(&(v_vms_total[0]),3);
    PCU_Add_Doubles(&t_err_total,1);
    PCU_Add_Doubles(&t_vms_total,1);
    PCU_Barrier();

    // print global efficiency
    if (!PCU_Comm_Self()) {
      printf("global efficiency (p,u,v,w,t): %f, %f, %f, %f, %f\n",
                               p_vms_total/p_err_total,
                               v_vms_total[0]/v_err_total[0],
                               v_vms_total[1]/v_err_total[1],
                               v_vms_total[2]/v_err_total[2],
                               t_vms_total/t_err_total);
    }

    // partition the dst mesh
    if(!PCU_Comm_Self())
      printf("start to partition the dst mesh\n");
    pPartitionOpts pOpts = PartitionOpts_new();
    PartitionOpts_setTotalNumParts(pOpts, PCU_Comm_Peers());
    PM_partition(dst_ppm, pOpts, progress);
    PartitionOpts_delete(pOpts);

    // write out fields
    apf::writeVtkFiles("efficiency", dst_m);
    Progress_delete(progress);
  }

} //end namespace

int main(int argc, char** argv) {
  MPI_Init(&argc, &argv);
  PCU_Comm_Init();
  PCU_Protect();
  if( argc != 4 ) {
    if(!PCU_Comm_Self())
      fprintf(stderr, "Usage: %s <refer_mesh.sms> <refer_restart_dir> <p_order>\n",argv[0]);
    exit(EXIT_FAILURE);
  }
  const char* referMeshFile   = argv[1];
  const char* referRestartDir = argv[2];
  integrationOrder = atoi(argv[3]);

  chefPhasta::initModelers();
  rstream rs = makeRStream();
  rstream ref_rs = makeRStream();
  grstream grs = makeGRStream();
  grstream ref_grs = makeGRStream();
  ph::Input ctrl;
  ph::Input ref_ctrl;
  ctrl.load("adapt.inp");
  ref_ctrl.load("adapt.inp");
  ref_ctrl.meshFileName = referMeshFile;
  ref_ctrl.restartFileName = referRestartDir;
  int step = ctrl.timeStepNumber;

  /* load the model and mesh */
  gmi_model* g = 0;
  apf::Mesh2* m = 0;
  setupChef(ctrl, step);
  chef::cook(g, m, ctrl, grs); // used to load model and mesh
  m->verify();

  gmi_model* ref_g = 0;
  apf::Mesh2* ref_m = 0;
  setupChef(ref_ctrl, step);
  chef::cook(ref_g, ref_m, ref_ctrl, ref_grs); // used to load model and mesh
  ref_m->verify();

  ctrl.rs = rs;
  ref_ctrl.rs = ref_rs;
  clearGRStream(grs);
  clearGRStream(ref_grs);

  /* calculate the efficiency */
  calculateEfficiency(ref_ctrl, ref_m, ctrl, m);

  clearRStream(rs);
  clearRStream(ref_rs);
  destroyGRStream(grs);
  destroyGRStream(ref_grs);
  destroyRStream(rs);
  destroyRStream(ref_rs);
  freeMesh(m);
  freeMesh(ref_m);
  chefPhasta::finalizeModelers();
  PCU_Comm_Free();
  MPI_Finalize();
}
