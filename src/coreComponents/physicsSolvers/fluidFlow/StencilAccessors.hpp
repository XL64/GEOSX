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
 * @file StencilAccessors.hpp
 */

#ifndef GEOSX_PHYSICSSOLVERS_FLUIDFLOW_STENCILACCESSORS_HPP_
#define GEOSX_PHYSICSSOLVERS_FLUIDFLOW_STENCILACCESSORS_HPP_

#include "mesh/ElementRegionManager.hpp"
#include "codingUtilities/traits.hpp"
#include "codingUtilities/Utilities.hpp"

#include <tuple>

namespace geosx
{

/**
 * @brief A struct to automatically construct and store element view accessors
 * @struct StencilAccessors
 * @tparam TRAITS the pack containing the types of the fields
 */
template< typename ... TRAITS >
class StencilAccessors
{
public:

  /**
   * @brief @return reference to the accessor for the given field
   * @tparam TRAIT the field trait type
   */
  template< typename TRAIT >
  auto get( TRAIT ) const
  {
    return std::get< traits::type_list_index< TRAIT, std::tuple< TRAITS ... > > >( m_accessors ).toNestedViewConst();
  }

  template< typename TRAIT >
  auto get() const
  {
    return std::get< traits::type_list_index< TRAIT, std::tuple< TRAITS ... > > >( m_accessors ).toNestedViewConst();
  }
  /**
   * @brief Constructor for the struct
   * @param[in] elemManager a reference to the elemRegionManager
   * @param[in] solverName the name of the solver creating the view accessors
   */
  StencilAccessors( ElementRegionManager const & elemManager,
                    string const & solverName )
  {
    forEachArgInTuple( std::tuple< TRAITS ... >{}, [&]( auto t, auto idx )
    {
      GEOSX_UNUSED_VAR( t );
      using TRAIT = TYPEOFREF( t );

      auto & acc = std::get< idx() >( m_accessors );
      acc = elemManager.constructExtrinsicAccessor< TRAIT >();
      acc.setName( solverName + "/accessors/" + TRAIT::key() );
    } );
  }

  /**
   * @brief Constructor for the struct
   * @param[in] elemManager a reference to the elemRegionManager
   * @param[in] solverName the name of the solver creating the view accessors
   * @param[in] regionNames the name of the solver target regions
   * @param[in] materialNames the name of the solver material names
   */
  StencilAccessors( ElementRegionManager const & elemManager,
                    string const & solverName,
                    arrayView1d< string const > const & regionNames,
                    arrayView1d< string const > const & materialNames )
  {
    if( materialNames.empty() )
    {
      return; // if the material model does not exist (i.e., no capillary pressure), do not do anything
    }
    forEachArgInTuple( std::tuple< TRAITS ... >{}, [&]( auto t, auto idx )
    {
      GEOSX_UNUSED_VAR( t );
      using TRAIT = TYPEOFREF( t );

      auto & acc = std::get< idx() >( m_accessors );
      bool const allowMissingViews = false;
      acc = elemManager.constructMaterialExtrinsicAccessor< TRAIT >( regionNames,
                                                                     materialNames,
                                                                     allowMissingViews );
      acc.setName( solverName + "/accessors/" + TRAIT::key() );
    } );
  }

private:

  /// the tuple storing all the accessors
  std::tuple< ElementRegionManager::ElementViewAccessor< traits::ViewTypeConst< typename TRAITS::type > > ... > m_accessors;
};

}

#endif //GEOSX_PHYSICSSOLVERS_FLUIDFLOW_STENCILACCESSORS_HPP_
