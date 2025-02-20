/*
 * ------------------------------------------------------------------------------------------------------------
 * SPDX-License-Identifier: LGPL-2.1-only
 *
 * Copyright (c) 2018-2020 Lawrence Livermore National Security LLC
 * Copyright (c) 2018-2020 The Board of Trustees of the Leland Stanford Junior University
 * Copyright (c) 2018-2020 TotalEnergies
 * Copyright (c) 2019-     GEOSX Contributors
 * All rights reserved
 *
 * See top level LICENSE, COPYRIGHT, CONTRIBUTORS, NOTICE, and ACKNOWLEDGEMENTS files for details.
 * ------------------------------------------------------------------------------------------------------------
 */

/**
 * @file FlowSolverBase.cpp
 */

#include "FlowSolverBase.hpp"

#include "constitutive/ConstitutivePassThru.hpp"
#include "constitutive/permeability/PermeabilityExtrinsicData.hpp"
#include "discretizationMethods/NumericalMethodsManager.hpp"
#include "fieldSpecification/AquiferBoundaryCondition.hpp"
#include "fieldSpecification/EquilibriumInitialCondition.hpp"
#include "fieldSpecification/FieldSpecificationManager.hpp"
#include "finiteVolume/FiniteVolumeManager.hpp"
#include "finiteVolume/FluxApproximationBase.hpp"
#include "mainInterface/ProblemManager.hpp"
#include "mesh/DomainPartition.hpp"
#include "physicsSolvers/fluidFlow/FluxKernelsHelper.hpp"
#include "physicsSolvers/fluidFlow/FlowSolverBaseExtrinsicData.hpp"

namespace geosx
{

using namespace dataRepository;
using namespace constitutive;

template< typename POROUSWRAPPER_TYPE >
void execute1( POROUSWRAPPER_TYPE porousWrapper,
               CellElementSubRegion & subRegion,
               arrayView1d< real64 const > const & pressure,
               arrayView1d< real64 const > const & deltaPressure )
{
  forAll< parallelDevicePolicy<> >( subRegion.size(), [=] GEOSX_DEVICE ( localIndex const k )
  {
    for( localIndex q = 0; q < porousWrapper.numGauss(); ++q )
    {
      porousWrapper.updateStateFromPressure( k, q,
                                             pressure[k],
                                             deltaPressure[k] );
    }
  } );
}

template< typename POROUSWRAPPER_TYPE >
void execute2( POROUSWRAPPER_TYPE porousWrapper,
               SurfaceElementSubRegion & subRegion,
               arrayView1d< real64 const > const & pressure,
               arrayView1d< real64 const > const & deltaPressure,
               arrayView1d< real64 const > const & oldHydraulicAperture,
               arrayView1d< real64 const > const & newHydraulicAperture )
{
  forAll< parallelDevicePolicy<> >( subRegion.size(), [=] GEOSX_DEVICE ( localIndex const k )
  {
    for( localIndex q = 0; q < porousWrapper.numGauss(); ++q )
    {
      porousWrapper.updateStateFromPressureAndAperture( k, q,
                                                        pressure[k],
                                                        deltaPressure[k],
                                                        oldHydraulicAperture[k],
                                                        newHydraulicAperture[k] );
    }
  } );
}

FlowSolverBase::FlowSolverBase( string const & name,
                                Group * const parent ):
  SolverBase( name, parent ),
  m_fluidModelNames(),
  m_solidModelNames(),
  m_permeabilityModelNames(),
  m_poroElasticFlag( 0 ),
  m_coupledWellsFlag( 0 ),
  m_numDofPerCell( 0 ),
  m_fluxEstimate()
{
  this->registerWrapper( viewKeyStruct::discretizationString(), &m_discretizationName ).
    setInputFlag( InputFlags::REQUIRED ).
    setDescription( "Name of discretization object to use for this solver." );

  this->registerWrapper( viewKeyStruct::fluidNamesString(), &m_fluidModelNames ).
    setInputFlag( InputFlags::REQUIRED ).
    setSizedFromParent( 0 ).
    setDescription( "Names of fluid constitutive models for each region." );

  this->registerWrapper( viewKeyStruct::solidNamesString(), &m_solidModelNames ).
    setInputFlag( InputFlags::REQUIRED ).
    setSizedFromParent( 0 ).
    setDescription( "Names of solid constitutive models for each region." );

  this->registerWrapper( viewKeyStruct::permeabilityNamesString(), &m_permeabilityModelNames ).
    setInputFlag( InputFlags::REQUIRED ).
    setSizedFromParent( 0 ).
    setDescription( "Names of permeability constitutive models for each region." );

  this->registerWrapper( viewKeyStruct::inputFluxEstimateString(), &m_fluxEstimate ).
    setApplyDefaultValue( 1.0 ).
    setInputFlag( InputFlags::OPTIONAL ).
    setDescription( "Initial estimate of the input flux used only for residual scaling. This should be "
                    "essentially equivalent to the input flux * dt." );
}

void FlowSolverBase::registerDataOnMesh( Group & meshBodies )
{
  SolverBase::registerDataOnMesh( meshBodies );

  meshBodies.forSubGroups< MeshBody >( [&] ( MeshBody & meshBody )
  {
    MeshLevel & mesh = meshBody.getMeshLevel( 0 );

    forTargetSubRegions( mesh, [&]( localIndex const,
                                    ElementSubRegionBase & subRegion )
    {
      subRegion.registerExtrinsicData< extrinsicMeshData::flow::gravityCoefficient >( getName() ).
        setApplyDefaultValue( 0.0 );
    } );

    ElementRegionManager & elemManager = mesh.getElemManager();

    elemManager.forElementSubRegionsComplete< SurfaceElementSubRegion >( [&]( localIndex const,
                                                                              localIndex const,
                                                                              ElementRegionBase & region,
                                                                              SurfaceElementSubRegion & subRegion )
    {
      SurfaceElementRegion & faceRegion = dynamicCast< SurfaceElementRegion & >( region );

      subRegion.registerExtrinsicData< extrinsicMeshData::flow::gravityCoefficient >( getName() );

      subRegion.registerExtrinsicData< extrinsicMeshData::flow::aperture0 >( getName() ).
        setDefaultValue( faceRegion.getDefaultAperture() );

      subRegion.registerExtrinsicData< extrinsicMeshData::flow::hydraulicAperture >( getName() ).
        setDefaultValue( faceRegion.getDefaultAperture() );

    } );

    FaceManager & faceManager = mesh.getFaceManager();
    faceManager.registerExtrinsicData< extrinsicMeshData::flow::gravityCoefficient >( getName() ).
      setApplyDefaultValue( 0.0 );
  } );
}

void FlowSolverBase::postProcessInput()
{
  SolverBase::postProcessInput();
  checkModelNames( m_fluidModelNames, viewKeyStruct::fluidNamesString() );
  checkModelNames( m_solidModelNames, viewKeyStruct::solidNamesString() );
  checkModelNames( m_permeabilityModelNames, viewKeyStruct::permeabilityNamesString() );
}

void FlowSolverBase::initializePreSubGroups()
{
  SolverBase::initializePreSubGroups();

  DomainPartition & domain = this->getGroupByPath< DomainPartition >( "/Problem/domain" );

  // Validate perm and porosity models in regions (fluid models are validated by derived classes)
  domain.getMeshBodies().forSubGroups< MeshBody >( [&] ( MeshBody & meshBody )
  {
    MeshLevel & meshLevel = meshBody.getMeshLevel( 0 );
    validateModelMapping( meshLevel.getElemManager(), m_permeabilityModelNames );
    // validateModelMapping( meshLevel.getElemManager(), m_solidModelNames );
  } );

  // fill stencil targetRegions
  NumericalMethodsManager & numericalMethodManager = domain.getNumericalMethodManager();

  FiniteVolumeManager & fvManager = numericalMethodManager.getFiniteVolumeManager();

  if( fvManager.hasGroup< FluxApproximationBase >( m_discretizationName ) )
  {
    FluxApproximationBase & fluxApprox = fvManager.getFluxApproximation( m_discretizationName );
    array1d< string > & stencilTargetRegions = fluxApprox.targetRegions();
    array1d< string > & stencilCoeffModelNames = fluxApprox.coefficientModelNames();

    std::set< string > stencilTargetRegionsSet( stencilTargetRegions.begin(), stencilTargetRegions.end() );
    map< string, string > coeffModelNames;

    arrayView1d< const string > const & regionNames = targetRegionNames();

    for( localIndex i=0; i< regionNames.size(); i++ )
    {
      stencilTargetRegionsSet.insert( regionNames[i] );
      coeffModelNames[regionNames[i]] =  m_permeabilityModelNames[i];
    }

    stencilTargetRegions.clear();
    stencilCoeffModelNames.clear();
    for( auto const & targetRegion : stencilTargetRegionsSet )
    {
      stencilTargetRegions.emplace_back( targetRegion );
      stencilCoeffModelNames.emplace_back( coeffModelNames[targetRegion] );
    }
  }
}

void FlowSolverBase::initializePostInitialConditionsPreSubGroups()
{
  SolverBase::initializePostInitialConditionsPreSubGroups();

  DomainPartition & domain = this->getGroupByPath< DomainPartition >( "/Problem/domain" );;
  MeshLevel & mesh = domain.getMeshBody( 0 ).getMeshLevel( 0 );

  // Precompute solver-specific constant data (e.g. gravity-depth)
  precomputeData( mesh );
}

void FlowSolverBase::precomputeData( MeshLevel & mesh )
{
  FaceManager & faceManager = mesh.getFaceManager();
  real64 const gravVector[3] = LVARRAY_TENSOROPS_INIT_LOCAL_3( gravityVector() );

  forTargetSubRegions( mesh, [&]( localIndex const,
                                  ElementSubRegionBase & subRegion )
  {
    arrayView2d< real64 const > const elemCenter = subRegion.getElementCenter();

    arrayView1d< real64 > const gravityCoef =
      subRegion.getExtrinsicData< extrinsicMeshData::flow::gravityCoefficient >();

    forAll< parallelHostPolicy >( subRegion.size(), [=] ( localIndex const ei )
    {
      gravityCoef[ ei ] = LvArray::tensorOps::AiBi< 3 >( elemCenter[ ei ], gravVector );
    } );
  } );

  {
    arrayView2d< real64 const > const faceCenter = faceManager.faceCenter();

    arrayView1d< real64 > const gravityCoef =
      faceManager.getExtrinsicData< extrinsicMeshData::flow::gravityCoefficient >();

    forAll< parallelHostPolicy >( faceManager.size(), [=] ( localIndex const kf )
    {
      gravityCoef[ kf ] = LvArray::tensorOps::AiBi< 3 >( faceCenter[ kf ], gravVector );
    } );
  }
}

FlowSolverBase::~FlowSolverBase() = default;

void FlowSolverBase::updatePorosityAndPermeability( CellElementSubRegion & subRegion,
                                                    localIndex const targetIndex ) const
{
  GEOSX_MARK_FUNCTION;

  arrayView1d< real64 const > const & pressure = subRegion.getExtrinsicData< extrinsicMeshData::flow::pressure >();
  arrayView1d< real64 const > const & deltaPressure = subRegion.getExtrinsicData< extrinsicMeshData::flow::deltaPressure >();

  CoupledSolidBase & porousSolid = subRegion.template getConstitutiveModel< CoupledSolidBase >( m_solidModelNames[targetIndex] );

  constitutive::ConstitutivePassThru< CoupledSolidBase >::execute( porousSolid, [=, &subRegion] ( auto & castedPorousSolid )
  {
    typename TYPEOFREF( castedPorousSolid ) ::KernelWrapper porousWrapper = castedPorousSolid.createKernelUpdates();

    execute1( porousWrapper, subRegion, pressure, deltaPressure );
  } );
}

void FlowSolverBase::updatePorosityAndPermeability( SurfaceElementSubRegion & subRegion,
                                                    localIndex const targetIndex ) const
{
  GEOSX_MARK_FUNCTION;

  arrayView1d< real64 const > const & pressure = subRegion.getExtrinsicData< extrinsicMeshData::flow::pressure >();
  arrayView1d< real64 const > const & deltaPressure = subRegion.getExtrinsicData< extrinsicMeshData::flow::deltaPressure >();

  arrayView1d< real64 const > const newHydraulicAperture = subRegion.getExtrinsicData< extrinsicMeshData::flow::hydraulicAperture >();
  arrayView1d< real64 const > const oldHydraulicAperture = subRegion.getExtrinsicData< extrinsicMeshData::flow::aperture0 >();

  CoupledSolidBase & porousSolid = subRegion.template getConstitutiveModel< CoupledSolidBase >( m_solidModelNames[targetIndex] );

  constitutive::ConstitutivePassThru< CompressibleSolidBase >::execute( porousSolid, [=, &subRegion] ( auto & castedPorousSolid )
  {
    typename TYPEOFREF( castedPorousSolid ) ::KernelWrapper porousWrapper = castedPorousSolid.createKernelUpdates();

    execute2( porousWrapper, subRegion, pressure, deltaPressure, oldHydraulicAperture, newHydraulicAperture );

  } );
}


std::vector< string > FlowSolverBase::getConstitutiveRelations( string const & regionName ) const
{
  // TODO IS THIS EVER USED? WHAT FOR?

  localIndex const regionIndex = this->targetRegionIndex( regionName );

  std::vector< string > rval{ m_solidModelNames[regionIndex], m_fluidModelNames[regionIndex] };

  return rval;
}

void FlowSolverBase::findMinMaxElevationInEquilibriumTarget( DomainPartition & domain, // cannot be const...
                                                             std::map< string, localIndex > const & equilNameToEquilId,
                                                             arrayView1d< real64 > const & maxElevation,
                                                             arrayView1d< real64 > const & minElevation ) const
{
  array1d< real64 > localMaxElevation( equilNameToEquilId.size() );
  array1d< real64 > localMinElevation( equilNameToEquilId.size() );
  localMaxElevation.setValues< parallelHostPolicy >( -1e99 );
  localMinElevation.setValues< parallelHostPolicy >( 1e99 );

  FieldSpecificationManager & fsManager = FieldSpecificationManager::getInstance();

  fsManager.apply< EquilibriumInitialCondition >( 0.0,
                                                  domain,
                                                  "ElementRegions",
                                                  EquilibriumInitialCondition::catalogName(),
                                                  [&] ( EquilibriumInitialCondition const & fs,
                                                        string const &,
                                                        SortedArrayView< localIndex const > const & targetSet,
                                                        Group & subRegion,
                                                        string const & )
  {
    RAJA::ReduceMax< parallelDeviceReduce, real64 > targetSetMaxElevation( -1e99 );
    RAJA::ReduceMin< parallelDeviceReduce, real64 > targetSetMinElevation( 1e99 );

    arrayView2d< real64 const > const elemCenter =
      subRegion.getReference< array2d< real64 > >( ElementSubRegionBase::viewKeyStruct::elementCenterString() );

    forAll< parallelDevicePolicy<> >( targetSet.size(), [=] GEOSX_HOST_DEVICE ( localIndex const i )
    {
      localIndex const k = targetSet[i];
      targetSetMaxElevation.max( elemCenter[k][2] );
      targetSetMinElevation.min( elemCenter[k][2] );
    } );

    localIndex const equilIndex = equilNameToEquilId.at( fs.getName() );
    localMaxElevation[equilIndex] = LvArray::math::max( targetSetMaxElevation.get(), localMaxElevation[equilIndex] );
    localMinElevation[equilIndex] = LvArray::math::min( targetSetMinElevation.get(), localMinElevation[equilIndex] );

  } );

  MpiWrapper::allReduce( localMaxElevation.data(),
                         maxElevation.data(),
                         localMaxElevation.size(),
                         MpiWrapper::getMpiOp( MpiWrapper::Reduction::Max ),
                         MPI_COMM_GEOSX );
  MpiWrapper::allReduce( localMinElevation.data(),
                         minElevation.data(),
                         localMinElevation.size(),
                         MpiWrapper::getMpiOp( MpiWrapper::Reduction::Min ),
                         MPI_COMM_GEOSX );
}

void FlowSolverBase::saveAquiferConvergedState( real64 const & time,
                                                real64 const & dt,
                                                DomainPartition & domain )
{
  GEOSX_MARK_FUNCTION;

  FieldSpecificationManager & fsManager = FieldSpecificationManager::getInstance();
  MeshLevel const & mesh = domain.getMeshBody( 0 ).getMeshLevel( 0 );

  NumericalMethodsManager const & numericalMethodManager = domain.getNumericalMethodManager();
  FiniteVolumeManager const & fvManager = numericalMethodManager.getFiniteVolumeManager();
  FluxApproximationBase const & fluxApprox = fvManager.getFluxApproximation( m_discretizationName );
  ElementRegionManager const & elemManager = mesh.getElemManager();

  // This step requires three passes:
  //    - First we count the number of individual aquifers
  //    - Second we loop over all the stencil entries to compute the sum of aquifer influxes
  //    - Third we loop over the aquifers to save the sums of each individual aquifer

  // Step 1: count individual aquifers

  std::map< string, localIndex > aquiferNameToAquiferId;
  localIndex aquiferCounter = 0;

  fsManager.forSubGroups< AquiferBoundaryCondition >( [&] ( AquiferBoundaryCondition const & bc )
  {
    aquiferNameToAquiferId[bc.getName()] = aquiferCounter;
    aquiferCounter++;
  } );

  // Step 2: sum the aquifer fluxes for each individual aquifer

  array1d< real64 > globalSumFluxes( aquiferNameToAquiferId.size() );
  array1d< real64 > localSumFluxes( aquiferNameToAquiferId.size() );

  fsManager.apply< AquiferBoundaryCondition >( time + dt,
                                               domain,
                                               "faceManager",
                                               AquiferBoundaryCondition::catalogName(),
                                               [&] ( AquiferBoundaryCondition const & bc,
                                                     string const & setName,
                                                     SortedArrayView< localIndex const > const &,
                                                     Group &,
                                                     string const & )
  {
    BoundaryStencil const & stencil = fluxApprox.getStencil< BoundaryStencil >( mesh, setName );
    if( stencil.size() == 0 )
    {
      return;
    }

    AquiferBoundaryCondition::KernelWrapper aquiferBCWrapper = bc.createKernelWrapper();

    using namespace extrinsicMeshData::flow;

    ElementRegionManager::ElementViewAccessor< arrayView1d< real64 const > >
    m_pressure = elemManager.constructExtrinsicAccessor< pressure >();
    m_pressure.setName( getName() + "/accessors/" + pressure::key() );

    ElementRegionManager::ElementViewAccessor< arrayView1d< real64 const > >
    m_deltaPressure = elemManager.constructExtrinsicAccessor< deltaPressure >();
    m_deltaPressure.setName( getName() + "/accessors/" + deltaPressure::key() );

    ElementRegionManager::ElementViewAccessor< arrayView1d< real64 const > >
    m_gravCoef = elemManager.constructExtrinsicAccessor< gravityCoefficient >();
    m_gravCoef.setName( getName() + "/accessors/" + gravityCoefficient::key() );

    real64 const targetSetSumFluxes =
      FluxKernelsHelper::AquiferBCKernel::sumFluxes( stencil,
                                                     aquiferBCWrapper,
                                                     m_pressure.toNestedViewConst(),
                                                     m_deltaPressure.toNestedViewConst(),
                                                     m_gravCoef.toNestedViewConst(),
                                                     time,
                                                     dt );

    localIndex const aquiferIndex = aquiferNameToAquiferId.at( bc.getName() );
    localSumFluxes[aquiferIndex] += targetSetSumFluxes;
  } );

  MpiWrapper::allReduce( localSumFluxes.data(),
                         globalSumFluxes.data(),
                         localSumFluxes.size(),
                         MpiWrapper::getMpiOp( MpiWrapper::Reduction::Sum ),
                         MPI_COMM_GEOSX );

  // Step 3: we are ready to save the summed fluxes for each individual aquifer

  fsManager.forSubGroups< AquiferBoundaryCondition >( [&] ( AquiferBoundaryCondition & bc )
  {
    localIndex const aquiferIndex = aquiferNameToAquiferId.at( bc.getName() );

    if( bc.getLogLevel() >= 1 )
    {
      GEOSX_LOG_RANK_0( GEOSX_FMT( string( "FlowSolverBase {}: at time {}s, " )
                                   + string( "the <{}> boundary condition '{}' produces a flux of {} kg (or moles if useMass=0). " ),
                                   getName(), time+dt, AquiferBoundaryCondition::catalogName(), bc.getName(), dt * globalSumFluxes[aquiferIndex] ) );
    }
    bc.saveConvergedState( dt * globalSumFluxes[aquiferIndex] );
  } );
}


} // namespace geosx
