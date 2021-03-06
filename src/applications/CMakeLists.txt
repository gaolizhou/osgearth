PROJECT(OSGEARTH_APPLICATIONS)

SET(OSGCORE_BUNDLED TRUE)

IF(NOT OSGCORE_BUNDLED)
	FIND_PACKAGE(OSGCORE)
	IF(OSGCORE_FOUND)
		INCLUDE(${OSGCORE_USE_FILE})
	ELSE(OSGCORE_FOUND)
		MESSAGE(ERROR "OSGCORE neeeded but NOT FOUND")
	ENDIF(OSGCORE_FOUND)
	SET(CMAKE_MODULE_PATH  ${PROJECT_SOURCE_DIR}/../../Macros)
ENDIF(NOT OSGCORE_BUNDLED)
SET(OPENSCENEGRAPH_APPLICATION_DIR ${PROJECT_SOURCE_DIR})


#OpenThreads, osg, osgDB and osgUtil are included elsewhere.
SET(TARGET_COMMON_LIBRARIES
    osgEarth
    osgEarthFeatures
    osgEarthUtil
    osgEarthSymbology
    osgEarthAnnotation
)

SET(TARGET_DEFAULT_PREFIX "application_")
SET(TARGET_DEFAULT_APPLICATION_FOLDER "Applications")

SET(TARGET_DEFAULT_LABEL_PREFIX "Tool")
SET(TARGET_DEFAULT_APPLICATION_FOLDER "Tools")
ADD_SUBDIRECTORY(osgearth_viewer)
ADD_SUBDIRECTORY(osgearth_seed)
ADD_SUBDIRECTORY(osgearth_package)
ADD_SUBDIRECTORY(osgearth_tfs)
ADD_SUBDIRECTORY(osgearth_boundarygen)
ADD_SUBDIRECTORY(osgearth_backfill)
ADD_SUBDIRECTORY(osgearth_overlayviewer)
ADD_SUBDIRECTORY(osgearth_version)
ADD_SUBDIRECTORY(osgearth_tileindex)
ADD_SUBDIRECTORY(osgearth_atlas)
ADD_SUBDIRECTORY(osgearth_conv)
ADD_SUBDIRECTORY(osgearth_3pv)

IF (Qt5Widgets_FOUND OR QT4_FOUND AND NOT ANDROID AND OSGEARTH_USE_QT AND OSGEARTH_QT_BUILD_LEGACY_WIDGETS)
    ADD_SUBDIRECTORY(osgearth_package_qt)
ENDIF()

IF(BUILD_OSGEARTH_EXAMPLES)
    SET(TARGET_DEFAULT_LABEL_PREFIX "Sample")
    SET(TARGET_DEFAULT_APPLICATION_FOLDER "Samples")
    ADD_SUBDIRECTORY(osgearth_clamp)
    ADD_SUBDIRECTORY(osgearth_manip)
    ADD_SUBDIRECTORY(osgearth_toc)
    ADD_SUBDIRECTORY(osgearth_createtile)
    ADD_SUBDIRECTORY(osgearth_elevation)
    ADD_SUBDIRECTORY(osgearth_features)
    ADD_SUBDIRECTORY(osgearth_featureinfo)
    ADD_SUBDIRECTORY(osgearth_featurefilter)
    ADD_SUBDIRECTORY(osgearth_los)
    ADD_SUBDIRECTORY(osgearth_terrainprofile)
    ADD_SUBDIRECTORY(osgearth_map)
    ADD_SUBDIRECTORY(osgearth_annotation)
    ADD_SUBDIRECTORY(osgearth_tracks)
    ADD_SUBDIRECTORY(osgearth_transform)
    ADD_SUBDIRECTORY(osgearth_horizon)
    ADD_SUBDIRECTORY(osgearth_http)

    IF(NOT ${OPENSCENEGRAPH_VERSION} VERSION_LESS "2.9.6")
        ADD_SUBDIRECTORY(osgearth_featureeditor)
    ENDIF()

    ADD_SUBDIRECTORY(osgearth_measure)
    ADD_SUBDIRECTORY(osgearth_controls)
    ADD_SUBDIRECTORY(osgearth_shadercomp)
    ADD_SUBDIRECTORY(osgearth_tilesource)
    ADD_SUBDIRECTORY(osgearth_imageoverlay)
    ADD_SUBDIRECTORY(osgearth_city)
    ADD_SUBDIRECTORY(osgearth_graticule)
    ADD_SUBDIRECTORY(osgearth_featurequery)
    ADD_SUBDIRECTORY(osgearth_occlusionculling)
    ADD_SUBDIRECTORY(osgearth_colorfilter)
    ADD_SUBDIRECTORY(osgearth_sequencecontrol)
    ADD_SUBDIRECTORY(osgearth_minimap)
    ADD_SUBDIRECTORY(osgearth_sharedlayer)
    ADD_SUBDIRECTORY(osgearth_mrt)
    ADD_SUBDIRECTORY(osgearth_fog)
    ADD_SUBDIRECTORY(osgearth_shadergen)
    ADD_SUBDIRECTORY(osgearth_clipplane)
    ADD_SUBDIRECTORY(osgearth_cache_test)
    ADD_SUBDIRECTORY(osgearth_pick)
    ADD_SUBDIRECTORY(osgearth_wfs)
    ADD_SUBDIRECTORY(osgearth_datetime)
    ADD_SUBDIRECTORY(osgearth_pagingtest)
    ADD_SUBDIRECTORY(osgearth_xfbtest)
    ADD_SUBDIRECTORY(osgearth_ephemeris)
    ADD_SUBDIRECTORY(osgearth_computerangecallback)
    ADD_SUBDIRECTORY(osgearth_splat)
    ADD_SUBDIRECTORY(osgearth_skyview)
    ADD_SUBDIRECTORY(osgearth_server)
    ADD_SUBDIRECTORY(osgearth_deformation)


    IF (Qt5Widgets_FOUND OR QT4_FOUND AND NOT ANDROID AND OSGEARTH_USE_QT)
        ADD_SUBDIRECTORY(osgearth_qt_simple)
        ADD_SUBDIRECTORY(osgearth_qt_windows)
    ENDIF()

    IF(SILVERLINING_FOUND)
        ADD_SUBDIRECTORY(osgearth_silverlining)
    ENDIF(SILVERLINING_FOUND)

    IF(TRITON_FOUND)
        ADD_SUBDIRECTORY(osgearth_triton)
    ENDIF(TRITON_FOUND)
ENDIF(BUILD_OSGEARTH_EXAMPLES)

