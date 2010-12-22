/*! \file Peridigm.cpp
 *
 * File containing main class for Peridigm: A parallel, multi-physics,
 * peridynamics simulation code.
 */

// ***********************************************************************
//
//                             Peridigm
//                 Copyright (2009) Sandia Corporation
//
// Under the terms of Contract DE-AC04-94AL85000 with Sandia Corporation,
// the U.S. Government retains certain rights in this software.
//
// This library is free software; you can redistribute it and/or modify
// it under the terms of the GNU Lesser General Public License as
// published by the Free Software Foundation; either version 2.1 of the
// License, or (at your option) any later version.
//
// This library is distributed in the hope that it will be useful, but
// WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
// Lesser General Public License for more details.
//
// You should have received a copy of the GNU Lesser General Public
// License along with this library; if not, write to the Free Software
// Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307
// USA
// 
// Questions? 
// David J. Littlewood   djlittl@sandia.gov 
// John A. Mitchell      jamitch@sandia.gov
// Michael L. Parks      mlparks@sandia.gov
// Stewart A. Silling    sasilli@sandia.gov
//
// ***********************************************************************

#include <iostream>
#include <vector>
#include <map>

#include <Epetra_Import.h>
#include <Teuchos_VerboseObject.hpp>

#include "Peridigm.hpp"
#include "Peridigm_DiscretizationFactory.hpp"
#include "Peridigm_OutputManager_VTK_XML.hpp"
#include "contact/Peridigm_ContactModel.hpp"
#include "contact/Peridigm_ShortRangeForceContactModel.hpp"
#include "materials/Peridigm_LinearElasticIsotropicMaterial.hpp"
#include "materials/Peridigm_IsotropicElasticPlasticMaterial.hpp"
#include "PdGridData.h"
#include "PdQuickGrid.h"
#include "PdZoltan.h"

using namespace std;

PeridigmNS::Peridigm::Peridigm(const Teuchos::RCP<const Epetra_Comm>& comm,
                   const Teuchos::RCP<Teuchos::ParameterList>& params)
  : computeContact(false),
    contactSearchRadius(0.0),
    contactSearchFrequency(0)
{

  peridigmComm = comm;
  peridigmParams = params;

  out = Teuchos::VerboseObjectBase::getDefaultOStream();

  // Instantiate materials using provided parameters
  instantiateMaterials();

  // Read mesh from disk or generate using geometric primatives.
  // All maps are generated here
  Teuchos::RCP<Teuchos::ParameterList> discParams = Teuchos::rcp(&(peridigmParams->sublist("Problem").sublist("Discretization")), false);
  DiscretizationFactory discFactory(discParams);
  Teuchos::RCP<AbstractDiscretization> peridigmDisc = discFactory.create(peridigmComm);
  initializeDiscretization(peridigmDisc);

  // Instantiate data manager
  dataManager = Teuchos::rcp(new PeridigmNS::DataManager);
  dataManager->setScalarMap(oneDimensionalOverlapMap);
  dataManager->setVector3DMap(threeDimensionalOverlapMap);
  dataManager->setBondMap(bondMap);
  // Create a master list of variable specs
  Teuchos::RCP< std::vector<Field_NS::FieldSpec> > variableSpecs = Teuchos::rcp(new std::vector<Field_NS::FieldSpec>);
  // Start with the specs used by Peridigm
  variableSpecs->push_back(Field_NS::VOLUME);
  variableSpecs->push_back(Field_NS::COORD3D);
  variableSpecs->push_back(Field_NS::DISPL3D);
  variableSpecs->push_back(Field_NS::CURCOORD3D);
  variableSpecs->push_back(Field_NS::VELOC3D);
  variableSpecs->push_back(Field_NS::FORCE3D);
  // Add the variable specs requested by each material
  for(unsigned int i=0; i<materials->size() ; ++i){
    Teuchos::RCP< std::vector<Field_NS::FieldSpec> > matVariableSpecs = (*materials)[i]->VariableSpecs();
    for(unsigned int j=0 ; j<matVariableSpecs->size() ; ++j)
      variableSpecs->push_back((*matVariableSpecs)[j]);
  }
  // Allocalte data in the dataManager
  dataManager->allocateData(variableSpecs);

  // Fill the dataManager with data from the discretization
  dataManager->getData(Field_NS::VOLUME, Field_NS::FieldSpec::STEP_NONE)->Import(*(peridigmDisc->getCellVolume()), *oneDimensionalMapToOneDimensionalOverlapMapImporter, Insert);
  dataManager->getData(Field_NS::COORD3D, Field_NS::FieldSpec::STEP_NONE)->Import(*x, *threeDimensionalMapToThreeDimensionalOverlapMapImporter, Insert);
  dataManager->getData(Field_NS::CURCOORD3D, Field_NS::FieldSpec::STEP_N)->Import(*x, *threeDimensionalMapToThreeDimensionalOverlapMapImporter, Insert);
  dataManager->getData(Field_NS::CURCOORD3D, Field_NS::FieldSpec::STEP_NP1)->Import(*x, *threeDimensionalMapToThreeDimensionalOverlapMapImporter, Insert);

  // apply initial velocities
  applyInitialVelocities();

  // Setup contact
  initializeContact();

  // Initialize the workset
  initializeWorkset();

  // Create the model evaluator
  modelEvaluator = Teuchos::rcp(new PeridigmNS::ModelEvaluator(materials, comm));

  // Initialize material models
  initializeMaterials();

  // Initialize output manager
  initializeOutputManager();
}

void PeridigmNS::Peridigm::instantiateMaterials() {

  // Extract problem parameters sublist
  Teuchos::RCP<Teuchos::ParameterList> problemParams = Teuchos::rcp(&(peridigmParams->sublist("Problem")),false);

  materials = Teuchos::rcp(new std::vector< Teuchos::RCP<const PeridigmNS::Material> >()); 

  // Instantiate material objects
  //! \todo Move creation of material models to material model factory
  TEST_FOR_EXCEPT_MSG(!problemParams->isSublist("Material"), "Material parameters not specified!");
  Teuchos::ParameterList & materialParams = problemParams->sublist("Material");
  Teuchos::ParameterList::ConstIterator it;
  for(it = materialParams.begin() ; it != materialParams.end() ; it++){
    const string & name = it->first;
    Teuchos::ParameterList & matParams = materialParams.sublist(name);
    // Insert solver timestep into matParams. Some material models (e.g., viscoelastic) need to know timestep
    Teuchos::RCP<Teuchos::ParameterList> solverParams = Teuchos::rcp(&(peridigmParams->sublist("Solver")),false);
    //! \todo Generalize for other solvers besides Verlet
    Teuchos::RCP<Teuchos::ParameterList> verletPL = sublist(solverParams, "Verlet", true);
    matParams.set("Fixed dt", verletPL->get("Fixed dt", 1.0) );
    Teuchos::RCP<Material> material;
    if(name == "Linear Elastic" || name == "Elastic Plastic"){
      if(name == "Linear Elastic")
        material = Teuchos::rcp(new LinearElasticIsotropicMaterial(matParams) );
      else if(name == "Elastic Plastic")
        material = Teuchos::rcp(new IsotropicElasticPlasticMaterial(matParams) );
      materials->push_back( Teuchos::rcp_implicit_cast<Material>(material) );
    }
    else {
      string invalidMaterial("Unrecognized material model: ");
      invalidMaterial += name;
      invalidMaterial += ", must be Linear Elastic or Elastic Plastic";
      TEST_FOR_EXCEPT_MSG(true, invalidMaterial);
    }
  }
  TEST_FOR_EXCEPT_MSG(materials->size() == 0, "No material models created!");

}

void PeridigmNS::Peridigm::initializeMaterials() {

  std::vector< Teuchos::RCP<const PeridigmNS::Material> >::const_iterator matIt;

  for(matIt = materials->begin() ; matIt != materials->end() ; matIt++){
    double dt = 0.0;
    (*matIt)->initialize(dt,
                         neighborhoodData->NumOwnedPoints(),
                         neighborhoodData->OwnedIDs(),
                         neighborhoodData->NeighborhoodList(),
                         *dataManager);
  }
}

void PeridigmNS::Peridigm::initializeDiscretization(Teuchos::RCP<AbstractDiscretization> peridigmDisc) {

  // oneDimensionalMap
  // used for cell volumes and scalar constitutive data
  oneDimensionalMap = peridigmDisc->getMap(1); 

  // oneDimensionalOverlapMap
  // used for cell volumes and scalar constitutive data
  // includes ghosts
  oneDimensionalOverlapMap = peridigmDisc->getOverlapMap(1);

  // threeDimensionalMap
  // used for positions, displacements, velocities and vector constitutive data
  threeDimensionalMap = peridigmDisc->getMap(3);

  // threeDimensionalOverlapMap
  // used for positions, displacements, velocities and vector constitutive data
  // includes ghosts
  threeDimensionalOverlapMap = peridigmDisc->getOverlapMap(3);

  // bondConstitutiveDataMap
  // a non-overlapping map used for storing constitutive data on bonds
  bondMap = peridigmDisc->getBondMap();

  // Create x, u, y, v, a, and force vectors
  x = peridigmDisc->getInitialX();                                 // initial positions
  u = Teuchos::rcp(new Epetra_Vector(*threeDimensionalMap));       // displacement
  y = Teuchos::rcp(new Epetra_Vector(*threeDimensionalMap));       // current positions
  v = Teuchos::rcp(new Epetra_Vector(*threeDimensionalMap));       // velocities
  a =  Teuchos::rcp(new Epetra_Vector(*threeDimensionalMap));      // accelerations
  force =  Teuchos::rcp(new Epetra_Vector(*threeDimensionalMap));  // force

  // Create the importers
  oneDimensionalMapToOneDimensionalOverlapMapImporter = Teuchos::rcp(new Epetra_Import(*oneDimensionalOverlapMap, *oneDimensionalMap));
  threeDimensionalMapToThreeDimensionalOverlapMapImporter = Teuchos::rcp(new Epetra_Import(*threeDimensionalOverlapMap, *threeDimensionalMap));

  // get the neighborlist from the discretization
  neighborhoodData = peridigmDisc->getNeighborhoodData();
}

void PeridigmNS::Peridigm::applyInitialVelocities() {

  TEST_FOR_EXCEPT_MSG(!threeDimensionalMap->SameAs(v->Map()), 
                      "Peridigm::applyInitialVelocities():  Inconsistent velocity vector map.\n");

  Teuchos::ParameterList& problemParams = peridigmParams->sublist("Problem");
  Teuchos::ParameterList& bcParams = problemParams.sublist("Boundary Conditions");
  Teuchos::ParameterList::ConstIterator it;

  // get the node sets
  map< string, vector<int> > nodeSets;
  for(it = bcParams.begin() ; it != bcParams.end() ; it++){
	const string& name = it->first;
    // \todo Change input deck so that node sets are parameter lists, not parameters, to avoid this ridiculous search.
	size_t position = name.find("Node Set");
	if(position != string::npos){
	  stringstream ss(Teuchos::getValue<string>(it->second));
	  vector<int> nodeList;
	  int nodeID;
	  while(ss.good()){
		ss >> nodeID;
		nodeList.push_back(nodeID);
	  }
	  nodeSets[name] = nodeList;
	}
  }

  // apply the initial conditions
  for(it = bcParams.begin() ; it != bcParams.end() ; it++){
	const string & name = it->first;
	size_t position = name.find("Initial Velocity");
	if(position != string::npos){
	  Teuchos::ParameterList & boundaryConditionParams = Teuchos::getValue<Teuchos::ParameterList>(it->second);
	  string nodeSet = boundaryConditionParams.get<string>("Node Set");
	  string type = boundaryConditionParams.get<string>("Type");
	  string coordinate = boundaryConditionParams.get<string>("Coordinate");
	  double value = boundaryConditionParams.get<double>("Value");

	  int coord = 0;
	  if(coordinate == "y" || coordinate == "Y")
		coord = 1;
	  if(coordinate == "z" || coordinate == "Z")
		coord = 2;

	  // apply initial velocity boundary conditions
	  // to locally-owned nodes
	  vector<int> & nodeList = nodeSets[nodeSet];
	  for(unsigned int i=0 ; i<nodeList.size() ; i++){
		int localNodeID = threeDimensionalMap->LID(nodeList[i]);
		if(localNodeID != -1)
		  (*v)[localNodeID*3 + coord] = value;
	  }
	}
  }
}

void PeridigmNS::Peridigm::initializeContact() {

  // Extract problem parameters sublist
  Teuchos::RCP<Teuchos::ParameterList> problemParams = Teuchos::rcp(&(peridigmParams->sublist("Problem")),false);

  // Extract discretization parameters sublist
  Teuchos::RCP<Teuchos::ParameterList> discParams = Teuchos::rcp(&(problemParams->sublist("Discretization")), false);

  // Assume no contact
  computeContact = false;
  contactSearchRadius = 0.0;
  contactSearchFrequency = 0;

  // Set up contact, if requested by user
  if(problemParams->isSublist("Contact")){
    Teuchos::ParameterList & contactParams = problemParams->sublist("Contact");
    computeContact = true;
    if(!contactParams.isParameter("Search Radius"))
      TEST_FOR_EXCEPTION(true, Teuchos::Exceptions::InvalidParameter, "Contact parameter \"Search Radius\" not specified.");
    contactSearchRadius = contactParams.get<double>("Search Radius");
    if(!contactParams.isParameter("Search Frequency"))
      TEST_FOR_EXCEPTION(true, Teuchos::Exceptions::InvalidParameter, "Contact parameter \"Search Frequency\" not specified.");
    contactSearchFrequency = contactParams.get<int>("Search Frequency");
  }

  // Instantiate contact models
  //! \todo Move creation of contact models to contact model factory
  contactModels = Teuchos::rcp(new std::vector<Teuchos::RCP<const PeridigmNS::ContactModel> >);
  if(computeContact){
    Teuchos::ParameterList & contactParams = problemParams->sublist("Contact");
    Teuchos::ParameterList::ConstIterator it;
    for(it = contactParams.begin() ; it != contactParams.end() ; it++){
      const string & name = it->first;
      if(contactParams.isSublist(name)){
        Teuchos::ParameterList & contactModelParams = contactParams.sublist(name);
        // Add the horizon to the contact model parameters, if needed
        if(!contactModelParams.isParameter("Horizon"))
          contactModelParams.set("Horizon", discParams->get<double>("Horizon"));
        Teuchos::RCP<PeridigmNS::ContactModel> contactModel;
        if(name == "Short Range Force"){
          contactModel = Teuchos::rcp(new PeridigmNS::ShortRangeForceContactModel(contactModelParams) );
          contactModels->push_back( Teuchos::rcp_implicit_cast<PeridigmNS::ContactModel>(contactModel) );
        }
        else{
          string invalidContactModel("Unrecognized contact model: ");
          invalidContactModel += name;
          invalidContactModel += ", must be Short Range Force";
          TEST_FOR_EXCEPT_MSG(true, invalidContactModel);
        }
      }
    }
  }

  // container for accelerations due to contact
  if(computeContact){
    contactForceOverlap = Teuchos::rcp(new Epetra_Vector(*threeDimensionalOverlapMap));
    contactNeighborhoodData = Teuchos::rcp(new PeridigmNS::NeighborhoodData);
    updateContactNeighborList();
  }
}

void PeridigmNS::Peridigm::initializeWorkset() {
  workset = Teuchos::rcp(new PHAL::Workset);
  workset->contactForceOverlap = contactForceOverlap;
  Teuchos::RCP<double> timeStep = Teuchos::rcp(new double);
  *timeStep = 0.0;
  workset->timeStep = timeStep;
  workset->neighborhoodData = neighborhoodData;
  workset->contactNeighborhoodData = contactNeighborhoodData;
  workset->dataManager = dataManager;
  workset->materials = materials;
  workset->contactModels = contactModels;
  workset->myPID = -1;
}

void PeridigmNS::Peridigm::initializeOutputManager() {

  bool active = false;
  Teuchos::RCP<Teuchos::ParameterList> outputParams;

  if (peridigmParams->isSublist("Output")) {
    active = true;
    outputParams  = Teuchos::rcp(&(peridigmParams->sublist("Output")),false);
    outputParams->set("NumProc", (int)(peridigmComm->NumProc()));
    outputParams->set("MyPID", (int)(peridigmComm->MyPID()));
  }

  if (active) {
    // Make the default format "VTK_XML"
    string outputFormat = outputParams->get("Output File Type", "VTK_XML");
    TEST_FOR_EXCEPTION( outputFormat != "VTK_XML",
                        std::invalid_argument,
                        "PeridigmNS::Peridigm: \"Output File Type\" must be \"VTK_XML\".");
    if (outputFormat == "VTK_XML")
       outputManager = Teuchos::rcp(new PeridigmNS::OutputManager_VTK_XML( outputParams ));
    else
      TEST_FOR_EXCEPTION( true, std::invalid_argument,"PeridigmNS::Peridigm::initializeOutputManager: \"Output File Type\" must be \"VTK_XML\".");

    // Query material models for their force state data descriptions
    forceStateDesc = Teuchos::rcp( new Teuchos::ParameterList() );
    for(unsigned int i=0; i<materials->size(); ++i){
      Teuchos::ParameterList& subList = forceStateDesc->sublist((*materials)[i]->Name());
      Teuchos::RCP< std::vector<Field_NS::FieldSpec> > matVariableSpecs = (*materials)[i]->VariableSpecs();
      for(unsigned int j=0 ; j<matVariableSpecs->size() ; ++j)
        subList.set( (*matVariableSpecs)[j].getLabel(), j);
    }

    // Initialize current time in this parameterlist
    forceStateDesc->set("Time", 0.0);
    // Set RCP to neighborlist
    forceStateDesc->set("Bond Family",neighborhoodData);
    // Ask OutputManager to write initial conditions to disk
    outputManager->write(x,u,v,a,force,dataManager,neighborhoodData,forceStateDesc);
  }

  //  verbose = problemParams->get("Verbose", false);

}

void PeridigmNS::Peridigm::execute() {

  // Use BLAS for local-only vector updates (BLAS-1)
  Epetra_BLAS blas;

  Teuchos::RCP<double> timeStep = Teuchos::rcp(new double);
  workset->timeStep = timeStep;

  // Copy data from mothership vectors to overlap vectors in data manager
  dataManager->getData(Field_NS::DISPL3D, Field_NS::FieldSpec::STEP_NP1)->Import(*u, *threeDimensionalMapToThreeDimensionalOverlapMapImporter, Insert);
  dataManager->getData(Field_NS::CURCOORD3D, Field_NS::FieldSpec::STEP_NP1)->Import(*y, *threeDimensionalMapToThreeDimensionalOverlapMapImporter, Insert);
  dataManager->getData(Field_NS::VELOC3D, Field_NS::FieldSpec::STEP_NP1)->Import(*v, *threeDimensionalMapToThreeDimensionalOverlapMapImporter, Insert);

  // evalModel() should be called by time integrator here...
  // For now, insert Verlet intergrator here
  Teuchos::RCP<Teuchos::ParameterList> solverParams = Teuchos::rcp(&(peridigmParams->sublist("Solver")),false);
  Teuchos::RCP<Teuchos::ParameterList> verletPL = sublist(solverParams, "Verlet", true);
  double t_initial = verletPL->get("Initial Time", 1.0);
  double t_current = t_initial;
  double dt        = verletPL->get("Fixed dt", 1.0);
  *timeStep = dt;
  double dt2 = dt/2.0;
  double t_final   = verletPL->get("Final Time", 1.0);
  int nsteps = (int)floor((t_final-t_initial)/dt);
  // Pointer index into sub-vectors for use with BLAS
  double *xptr, *uptr, *yptr, *vptr, *aptr;
  x->ExtractView( &xptr );
  u->ExtractView( &uptr );
  y->ExtractView( &yptr );
  v->ExtractView( &vptr );
  a->ExtractView( &aptr );
  int length = a->MyLength();

  bool rebal = false;

  for (int step=0;step<nsteps;step++) {

    // rebalance, if requested
    if(rebal)
      rebalance();

    // Do one step of velocity-Verlet

    // V^{n+1/2} = V^{n} + (dt/2)*A^{n}
    //blas.AXPY(const int N, const double ALPHA, const double *X, double *Y, const int INCX=1, const int INCY=1) const
    blas.AXPY(length, dt2, aptr, vptr, 1, 1);

    // Y^{n+1}   = X_{o} + U^{n} + (dt)*V^{n+1/2}
    // \todo Replace with blas call
    for(int i=0 ; i<y->MyLength() ; ++i)
      (*y)[i] = (*x)[i] + (*u)[i] + dt*(*v)[i];

    // Copy data from mothership vectors to overlap vectors in data manager
    dataManager->getData(Field_NS::DISPL3D, Field_NS::FieldSpec::STEP_NP1)->Import(*u, *threeDimensionalMapToThreeDimensionalOverlapMapImporter, Insert);
    dataManager->getData(Field_NS::CURCOORD3D, Field_NS::FieldSpec::STEP_NP1)->Import(*y, *threeDimensionalMapToThreeDimensionalOverlapMapImporter, Insert);
    dataManager->getData(Field_NS::VELOC3D, Field_NS::FieldSpec::STEP_NP1)->Import(*v, *threeDimensionalMapToThreeDimensionalOverlapMapImporter, Insert);

    // Update forces based on new positions
    modelEvaluator->evalModel(workset);

    // Reverse comm particle forces
    force->Export(*dataManager->getData(Field_NS::FORCE3D, Field_NS::FieldSpec::STEP_NP1), *threeDimensionalMapToThreeDimensionalOverlapMapImporter, Add);

    // fill the acceleration vector
    (*a) = (*force);
    // \todo Possibly move this functionality into ModelEvaluator.
    // \todo Generalize this for multiple materials
    double density = (*materials)[0]->Density();
    a->Scale(1.0/density);

    // U^{n+1}   = U^{n} + (dt)*V^{n+1/2}
    //blas.AXPY(const int N, const double ALPHA, const double *X, double *Y, const int INCX=1, const int INCY=1) const
    blas.AXPY(length, dt, vptr, uptr, 1, 1);

    // V^{n+1}   = V^{n+1/2} + (dt/2)*A^{n+1}
    //blas.AXPY(const int N, const double ALPHA, const double *X, double *Y, const int INCX=1, const int INCY=1) const
    blas.AXPY(length, dt2, aptr, vptr, 1, 1);

    t_current = t_initial + (step*dt);

// if (peridigmComm->MyPID() == 0)
// std::cout << "step = " << step << endl;

    // Update the contact configuration, if necessary
//    model->updateContact(currentSolution);
    // Only report status if Observer is active
//    if (!active) return;
    //cout << "PERIDIGM OBSERVER CALLED step=" <<  timeStepIter  << ",  time=" << stepper.getStepStatus().time << endl;
    // Set current time in this parameterlist
    forceStateDesc->set("Time", time);
    outputManager->write(x,u,v,a,force,dataManager,neighborhoodData,forceStateDesc);

    // swap state N and state NP1
    dataManager->updateState();
  }
}

void PeridigmNS::Peridigm::rebalance() {

  // Steps for rebalance:
  //
  // 1) Create a PdGridData object that contains, principally, the global IDs, 
  //    current positions, and cell volumes (neighborhood info is left uninitialized).
  // 2) Pass the PdGridData object to getLoadBalancedDiscretization(), which
  //    does the heavy lifting and returns a PdGridData object with complete
  //    neighborhood data.
  // 3) Create new maps from the rebalanced PdGridData object.  The required maps
  //    are the oneDimensionalMap, oneDimensionalOverlapMap, threeDimensionalMap,
  //    threeDimensionalOverlapMap, and the bondMap.
  // 4) Create Import objects that will transfer data from the old data containers
  //    to the new data containers.  This is straightforward in all cases except
  //    for the bond data.
  // 5) Import data from the old data structures to the new data structures, update
  //    the corresponding pointers so that everything uses the new data.

  ///// STEP 1 ////

  // Create a decomp object and fill necessary data for rebalance
  int myNumElements = oneDimensionalMap->NumMyElements();
  int dimension = 3;
  PdGridData decomp = PdQuickGrid::allocatePdGridData(myNumElements, dimension);

  // fill myGlobalIDs
  shared_ptr<int> myGlobalIDs(new int[myNumElements], PdQuickGrid::Deleter<int>());
  int* myGlobalIDsPtr = myGlobalIDs.get();
  int* gIDs = oneDimensionalMap->MyGlobalElements();
  for(int i=0 ; i<myNumElements ; ++i){
    myGlobalIDsPtr[i] = gIDs[i];
  }
  decomp.myGlobalIDs = myGlobalIDs;
  
  // fill myX and cellVolume
  shared_ptr<double> myX(new double[myNumElements*dimension], PdQuickGrid::Deleter<double>());
  double* myXPtr = myX.get();
  double* xPtr;
  x->ExtractView(&xPtr);
  shared_ptr<double> cellVolume(new double[myNumElements], PdQuickGrid::Deleter<double>());
  double* cellVolumePtr = cellVolume.get();
  double* cellVolumeOverlapPtr;
  dataManager->getData(Field_NS::VOLUME, Field_NS::FieldSpec::STEP_NONE)->ExtractView(&cellVolumeOverlapPtr);
  for(int i=0 ; i<myNumElements ; ++i){
    int oneDimensionalMapGlobalID = myGlobalIDsPtr[i];
    int oneDimensionalMapLocalID = oneDimensionalMap->LID(oneDimensionalMapGlobalID);
    int oneDimensionalOverlapMapLocalID = oneDimensionalOverlapMap->LID(oneDimensionalMapGlobalID);
    myXPtr[i*3] = xPtr[oneDimensionalMapLocalID];
    myXPtr[i*3+1] = xPtr[oneDimensionalMapLocalID+1];
    myXPtr[i*3+2] = xPtr[oneDimensionalMapLocalID+2];
    cellVolumePtr[i] = cellVolumeOverlapPtr[oneDimensionalOverlapMapLocalID];
  }  
  decomp.myX = myX;
  decomp.cellVolume = cellVolume;

  //// STEP 2 ////

  // rebalance
  decomp = getLoadBalancedDiscretization(decomp);

  //// STEP 3 ////

  Teuchos::RCP<Epetra_BlockMap> rebalancedOneDimensionalMap = Teuchos::rcp(new Epetra_BlockMap(PdQuickGrid::getOwnedMap(*peridigmComm, decomp, 1)));
  Teuchos::RCP<Epetra_BlockMap> rebalancedOneDimensionalOverlapMap = Teuchos::rcp(new Epetra_BlockMap(PdQuickGrid::getOverlapMap(*peridigmComm, decomp, 1)));
  Teuchos::RCP<Epetra_BlockMap> rebalancedThreeDimensionalMap = Teuchos::rcp(new Epetra_BlockMap(PdQuickGrid::getOwnedMap(*peridigmComm, decomp, 3)));
  Teuchos::RCP<Epetra_BlockMap> rebalancedThreeDimensionalOverlapMap = Teuchos::rcp(new Epetra_BlockMap(PdQuickGrid::getOverlapMap(*peridigmComm, decomp, 3)));

  // functionality from PdQuickGridDiscretization::createNeighborhoodData
  // \todo Eliminate code duplication here.
  // \todo Can we just overwrite neighborhoodData here?
  Teuchos::RCP<PeridigmNS::NeighborhoodData> rebalancedNeighborhoodData = Teuchos::rcp(new PeridigmNS::NeighborhoodData);
  rebalancedNeighborhoodData->SetNumOwned(decomp.numPoints);
  memcpy(rebalancedNeighborhoodData->OwnedIDs(), 
 		 PdQuickGrid::getLocalOwnedIds(decomp, *rebalancedOneDimensionalOverlapMap).get(),
 		 decomp.numPoints*sizeof(int));
  memcpy(rebalancedNeighborhoodData->NeighborhoodPtr(), 
 		 decomp.neighborhoodPtr.get(),
 		 decomp.numPoints*sizeof(int));
  rebalancedNeighborhoodData->SetNeighborhoodListSize(decomp.sizeNeighborhoodList);
  memcpy(rebalancedNeighborhoodData->NeighborhoodList(),
 		 PdQuickGrid::getLocalNeighborList(decomp, *rebalancedOneDimensionalOverlapMap).get(),
 		 decomp.sizeNeighborhoodList*sizeof(int));

  // functionality from PdQuickGridDiscretization constructor
  // determine the number of bonds based on the neighborhood data
  // THIS IS WRONG, NEED TO FIGURE OUT THE GLOBAL IDS SO THE IMPORTER WILL WORK.
  // \todo Fix rebalanced bond map.
  int numBonds = rebalancedNeighborhoodData->NeighborhoodListSize() - rebalancedNeighborhoodData->NumOwnedPoints();
  Teuchos::RCP<Epetra_BlockMap> rebalancedBondMap = Teuchos::rcp(new Epetra_BlockMap(-1, numBonds, 1, 0, *peridigmComm));

  //// STEP 4 ////

  Teuchos::RCP<const Epetra_Import> oneDimensionalMapImporter = Teuchos::rcp(new Epetra_Import(*rebalancedOneDimensionalMap, *oneDimensionalMap));
  Teuchos::RCP<const Epetra_Import> oneDimensionalOverlapMapImporter = Teuchos::rcp(new Epetra_Import(*rebalancedOneDimensionalOverlapMap, *oneDimensionalOverlapMap));
  Teuchos::RCP<const Epetra_Import> threeDimensionalMapImporter = Teuchos::rcp(new Epetra_Import(*rebalancedThreeDimensionalMap, *threeDimensionalMap));
  Teuchos::RCP<const Epetra_Import> threeDimensionalOverlapMapImporter = Teuchos::rcp(new Epetra_Import(*rebalancedThreeDimensionalOverlapMap, *threeDimensionalOverlapMap));
  Teuchos::RCP<const Epetra_Import> bondMapImporter = Teuchos::rcp(new Epetra_Import(*rebalancedBondMap, *bondMap));

  //// STEP 5 ////

  Teuchos::RCP<Epetra_Vector> rebalancedX = Teuchos::rcp(new Epetra_Vector(*rebalancedThreeDimensionalMap));
  rebalancedX->Import(*x, *threeDimensionalMapImporter, Insert);
  x = rebalancedX;

  Teuchos::RCP<Epetra_Vector> rebalancedU = Teuchos::rcp(new Epetra_Vector(*rebalancedThreeDimensionalMap));
  rebalancedU->Import(*u, *threeDimensionalMapImporter, Insert);
  u = rebalancedU;

  Teuchos::RCP<Epetra_Vector> rebalancedY = Teuchos::rcp(new Epetra_Vector(*rebalancedThreeDimensionalMap));
  rebalancedY->Import(*y, *threeDimensionalMapImporter, Insert);
  y = rebalancedY;

  Teuchos::RCP<Epetra_Vector> rebalancedV = Teuchos::rcp(new Epetra_Vector(*rebalancedThreeDimensionalMap));
  rebalancedV->Import(*v, *threeDimensionalMapImporter, Insert);
  v = rebalancedV;

  Teuchos::RCP<Epetra_Vector> rebalancedA = Teuchos::rcp(new Epetra_Vector(*rebalancedThreeDimensionalMap));
  rebalancedA->Import(*a, *threeDimensionalMapImporter, Insert);
  a = rebalancedA;

  Teuchos::RCP<Epetra_Vector> rebalancedForce = Teuchos::rcp(new Epetra_Vector(*rebalancedThreeDimensionalMap));
  rebalancedForce->Import(*force, *threeDimensionalMapImporter, Insert);
  force = rebalancedForce;

  // \todo DataManager rebalance here.
}

void PeridigmNS::Peridigm::updateContactNeighborList() {
  // initial implementation works in serial only
  TEST_FOR_EXCEPT_MSG(peridigmComm->NumProc()  != 1, "Contact is currently not enabled in parallel.\n");

// MLP: Most of this code should go away due to use of new maps
/* 
  // Create a decomp object and fill necessary data for rebalance
  int myNumElements = oneDimensionalMap->NumMyElements();
  int dimension = 3;
  PdGridData decomp = PdQuickGrid::allocatePdGridData(myNumElements, dimension);

  // fill myGlobalIDs
  shared_ptr<int> myGlobalIDs(new int[myNumElements], PdQuickGrid::Deleter<int>());
  int* myGlobalIDsPtr = myGlobalIDs.get();
  int* gIDs = oneDimensionalMap->MyGlobalElements();
  for(int i=0 ; i<myNumElements ; ++i){
    myGlobalIDsPtr[i] = gIDs[i];
  }
  decomp.myGlobalIDs = myGlobalIDs;

  // fill myX and cellVolume
  shared_ptr<double> myX(new double[myNumElements*dimension], PdQuickGrid::Deleter<double>());
  double* myXPtr = myX.get();
  double* solverXPtr;
  solverX->ExtractView(&solverXPtr);
  shared_ptr<double> cellVolume(new double[myNumElements], PdQuickGrid::Deleter<double>());
  double* cellVolumePtr = cellVolume.get();
  double* cellVolumeOverlapPtr;
  dataManager->getData(Field_NS::VOLUME, Field_NS::FieldSpec::STEP_NONE)->ExtractView(&cellVolumeOverlapPtr);
  for(int i=0 ; i<myNumElements ; ++i){
    int oneDimensionalMapGlobalID = myGlobalIDsPtr[i];
    int oneDimensionalOverlapMapLocalID = oneDimensionalOverlapMap->LID(oneDimensionalMapGlobalID);
    int threeDimensionalTwoEntryMapGlobalID = oneDimensionalMapGlobalID*3;
    int threeDimensionalTwoEntryMapLocalID = threeDimensionalTwoEntryMap->LID(threeDimensionalTwoEntryMapGlobalID);
    myXPtr[i*3] = solverXPtr[threeDimensionalTwoEntryMapLocalID];
    myXPtr[i*3+1] = solverXPtr[threeDimensionalTwoEntryMapLocalID+1];
    myXPtr[i*3+2] = solverXPtr[threeDimensionalTwoEntryMapLocalID+2];
    cellVolumePtr[i] = cellVolumeOverlapPtr[oneDimensionalOverlapMapLocalID];
  }
  decomp.myX = myX;
  decomp.cellVolume = cellVolume;

  // rebalance
  decomp = getLoadBalancedDiscretization(decomp);

  // big todo: shuffle data around based on decomp

  // execute contact search
  decomp = createAndAddNeighborhood(decomp, contactSearchRadius);

  // Copy the data in decomp into the contact neighbor list
  // Do not include points that are bonded

  vector<int> contactOwnedIDs;
  vector<int> contactNeighborhoodPtr;
  vector<int> contactNeighborhoodList;

  int searchListIndex = 0;
  int searchNumPoints = decomp.numPoints;
  int* searchNeighborhood = decomp.neighborhood.get();

  for(int iLID=0 ; iLID<searchNumPoints ; ++iLID){

    // find the cells that are bonded to this cell
    // store the corresponding local IDs in bondedNeighbors, which is a stl::list
    int* bondedNeighborhoodList = neighborhoodData->NeighborhoodList();
    int bondedListIndex = neighborhoodData->NeighborhoodPtr()[iLID];
    int numBondedNeighbors = bondedNeighborhoodList[bondedListIndex++];
    list<int> bondedNeighbors; // \todo reserve space here
    for(int i=0 ; i<numBondedNeighbors ; ++i){
      bondedNeighbors.push_back(bondedNeighborhoodList[bondedListIndex++]);
    }

    // loop over the cells found by the contact search
    // retain only those cells that are not bonded
        int searchNumNeighbors = searchNeighborhood[searchListIndex++];

    list<int>::iterator it;
    bool hasContact = false;
    int currentContactNeighborhoodPtr = 0;
        for(int iNeighbor=0 ; iNeighbor<searchNumNeighbors ; ++iNeighbor){
          int localNeighborID = searchNeighborhood[searchListIndex++];
      it = find(bondedNeighbors.begin(), bondedNeighbors.end(), localNeighborID);
      if(it == bondedNeighbors.end()){
        if(!hasContact){
          hasContact = true;
          contactOwnedIDs.push_back(iLID);
          currentContactNeighborhoodPtr = contactNeighborhoodList.size();
          contactNeighborhoodPtr.push_back(currentContactNeighborhoodPtr);
          contactNeighborhoodList.push_back(1);
        }
        else{
          contactNeighborhoodList[currentContactNeighborhoodPtr] += 1;
        }
        contactNeighborhoodList.push_back(localNeighborID);
      }
        }
  }

  TEST_FOR_EXCEPT_MSG(contactNeighborhoodPtr.size() != contactOwnedIDs.size(),
                      "Error, contactOwnedIDs and contactNeighborhoodPtr are different sizes in ModelEvaluator::updateContactNeighborList().\n");

  // copy the contact neighbor data into contactNeighborData
  contactNeighborhoodData->SetNumOwned(contactOwnedIDs.size());
  memcpy(contactNeighborhoodData->OwnedIDs(),
                 &contactOwnedIDs[0],
                 contactOwnedIDs.size()*sizeof(int));
  memcpy(contactNeighborhoodData->NeighborhoodPtr(),
                 &contactNeighborhoodPtr[0],
                 contactOwnedIDs.size()*sizeof(int));
  contactNeighborhoodData->SetNeighborhoodListSize(contactNeighborhoodList.size());
  memcpy(contactNeighborhoodData->NeighborhoodList(),
                 &contactNeighborhoodList[0],
                 contactNeighborhoodList.size()*sizeof(int));
*/

}
