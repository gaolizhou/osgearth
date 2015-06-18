/* -*-c++-*- */
/* osgEarth - Dynamic map generation toolkit for OpenSceneGraph
* Copyright 2008-2014 Pelican Mapping
* http://osgearth.org
*
* osgEarth is free software; you can redistribute it and/or modify
* it under the terms of the GNU Lesser General Public License as published by
* the Free Software Foundation; either version 2 of the License, or
* (at your option) any later version.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU Lesser General Public License for more details.
*
* You should have received a copy of the GNU Lesser General Public License
* along with this program.  If not, see <http://www.gnu.org/licenses/>
*/
#include "RexTerrainEngineNode"
#include "Shaders"
#include "QuickReleaseGLObjects"

#include <osgEarth/HeightFieldUtils>
#include <osgEarth/ImageUtils>
#include <osgEarth/Registry>
#include <osgEarth/Capabilities>
#include <osgEarth/VirtualProgram>
#include <osgEarth/ShaderFactory>
#include <osgEarth/MapModelChange>
#include <osgEarth/Progress>
#include <osgEarth/ShaderLoader>
#include <osgEarth/Utils>
#include <osgEarth/ObjectIndex>

#include <osg/Depth>
#include <osg/BlendFunc>
#include <osg/PatchParameter>
#include <osg/Multisample>
#include <osgUtil/RenderBin>

#define LC "[RexTerrainEngineNode] "

using namespace osgEarth::Drivers::RexTerrainEngine;
using namespace osgEarth;


// TODO: bins don't work with SSDK. No idea why. Disable until further notice.
#define USE_RENDER_BINS 1

//------------------------------------------------------------------------

namespace
{
    // adapter that lets RexTerrainEngineNode listen to Map events
    struct RexTerrainEngineNodeMapCallbackProxy : public MapCallback
    {
        RexTerrainEngineNodeMapCallbackProxy(RexTerrainEngineNode* node) : _node(node) { }
        osg::observer_ptr<RexTerrainEngineNode> _node;

        void onMapInfoEstablished( const MapInfo& mapInfo ) {
            osg::ref_ptr<RexTerrainEngineNode> node;
            if ( _node.lock(node) )
                node->onMapInfoEstablished( mapInfo );
        }

        void onMapModelChanged( const MapModelChange& change ) {
            osg::ref_ptr<RexTerrainEngineNode> node;
            if ( _node.lock(node) )
                node->onMapModelChanged( change );
        }
    };


    // Render bin for terrain surface geometry
    class SurfaceBin : public osgUtil::RenderBin
    {
    public:
        SurfaceBin()
        {
            this->setName( "oe.SurfaceBin" );
            this->setStateSet( new osg::StateSet() );
            this->setSortMode(SORT_FRONT_TO_BACK);
            OE_NOTICE << getName() << "ctor\n";
        }

        osg::Object* clone(const osg::CopyOp& copyop) const
        {
            return new SurfaceBin(*this, copyop);
        }

        SurfaceBin(const SurfaceBin& rhs, const osg::CopyOp& copy) :
            osgUtil::RenderBin(rhs, copy)
        {
        }
    };


    // Render bin for terrain control surface
    class LandCoverBin : public osgUtil::RenderBin
    {
    public:
        LandCoverBin()
        {
            this->setName( "oe.LandCoverBin" );
            this->setStateSet( new osg::StateSet() );
            //this->setSortMode(SORT_BACK_TO_FRONT);
        }

        osg::Object* clone(const osg::CopyOp& copyop) const
        {
            return new LandCoverBin(*this, copyop);
        }

        LandCoverBin(const LandCoverBin& rhs, const osg::CopyOp& copy) :
            osgUtil::RenderBin(rhs, copy)
        {
        }

        //void sort()
        //{
        //    osgUtil::RenderBin::sort();
        //    if ( getRenderLeafList().size() > 0 )
        //    {
        //        OE_NOTICE << LC << "LC Drawables = " << getRenderLeafList().size() << "\n";
        //    }
        //}
    };
}

//---------------------------------------------------------------------------

static Threading::ReadWriteMutex s_engineNodeCacheMutex;
//Caches the MapNodes that have been created
typedef std::map<UID, osg::observer_ptr<RexTerrainEngineNode> > EngineNodeCache;

static
EngineNodeCache& getEngineNodeCache()
{
    static EngineNodeCache s_cache;
    return s_cache;
}

void
RexTerrainEngineNode::registerEngine(RexTerrainEngineNode* engineNode)
{
    Threading::ScopedWriteLock exclusiveLock( s_engineNodeCacheMutex );
    getEngineNodeCache()[engineNode->_uid] = engineNode;
    OE_DEBUG << LC << "Registered engine " << engineNode->_uid << std::endl;
}

void
RexTerrainEngineNode::unregisterEngine( UID uid )
{
    Threading::ScopedWriteLock exclusiveLock( s_engineNodeCacheMutex );
    EngineNodeCache::iterator k = getEngineNodeCache().find( uid );
    if (k != getEngineNodeCache().end())
    {
        getEngineNodeCache().erase(k);
        OE_DEBUG << LC << "Unregistered engine " << uid << std::endl;
    }
}

// since this method is called in a database pager thread, we use a ref_ptr output
// parameter to avoid the engine node being destructed between the time we 
// return it and the time it's accessed; this could happen if the user removed the
// MapNode from the scene during paging.
void
RexTerrainEngineNode::getEngineByUID( UID uid, osg::ref_ptr<RexTerrainEngineNode>& output )
{
    Threading::ScopedReadLock sharedLock( s_engineNodeCacheMutex );
    EngineNodeCache::const_iterator k = getEngineNodeCache().find( uid );
    if (k != getEngineNodeCache().end())
        output = k->second.get();
}

UID
RexTerrainEngineNode::getUID() const
{
    return _uid;
}

//------------------------------------------------------------------------

RexTerrainEngineNode::ElevationChangedCallback::ElevationChangedCallback( RexTerrainEngineNode* terrain ):
_terrain( terrain )
{
    //nop
}

void
RexTerrainEngineNode::ElevationChangedCallback::onVisibleChanged( TerrainLayer* layer )
{
    _terrain->refresh(true); // true => force a dirty
}

//------------------------------------------------------------------------

RexTerrainEngineNode::RexTerrainEngineNode() :
TerrainEngineNode     ( ),
_terrain              ( 0L ),
_update_mapf          ( 0L ),
_tileCount            ( 0 ),
_tileCreationTime     ( 0.0 ),
_batchUpdateInProgress( false ),
_refreshRequired      ( false ),
_stateUpdateRequired  ( false )
{
    // unique ID for this engine:
    _uid = Registry::instance()->createUID();

    // always require elevation.
    _requireElevationTextures = true;

    // Register our render bins protos.
    {
        // Mutex because addRenderBinPrototype isn't thread-safe.
        Threading::ScopedMutexLock lock(_renderBinMutex);

        // generate uniquely named render bin prototypes for this engine:
        _surfaceRenderBinPrototype = new SurfaceBin();
        //_surfaceRenderBinPrototype->setName( "oe.SurfaceBin" ); //." << _uid );
        osgUtil::RenderBin::addRenderBinPrototype( _surfaceRenderBinPrototype->getName(), _surfaceRenderBinPrototype.get() );

        _landCoverRenderBinPrototype = new LandCoverBin();
        //_landCoverRenderBinPrototype->setName( "oe.LandCoverBin" ); //." << _uid );
        osgUtil::RenderBin::addRenderBinPrototype( _landCoverRenderBinPrototype->getName(), _landCoverRenderBinPrototype.get() );
    }

    // install an elevation callback so we can update elevation data
    _elevationCallback = new ElevationChangedCallback( this );
}

RexTerrainEngineNode::~RexTerrainEngineNode()
{
    unregisterEngine( _uid );

    osgUtil::RenderBin::removeRenderBinPrototype( _surfaceRenderBinPrototype.get() );
    osgUtil::RenderBin::removeRenderBinPrototype( _landCoverRenderBinPrototype.get() );

    if ( _update_mapf )
    {
        delete _update_mapf;
    }
}

void
RexTerrainEngineNode::preInitialize( const Map* map, const TerrainOptions& options )
{
    TerrainEngineNode::preInitialize( map, options );
    //nop.
}

void
RexTerrainEngineNode::postInitialize( const Map* map, const TerrainOptions& options )
{
    TerrainEngineNode::postInitialize( map, options );

    // Initialize the map frames. We need one for the update thread and one for the
    // cull thread. Someday we can detect whether these are actually the same thread
    // (depends on the viewer's threading mode).
    _update_mapf = new MapFrame( map, Map::ENTIRE_MODEL, "mp-update" );

    // merge in the custom options:
    _terrainOptions.merge( options );

    if ( _terrainOptions.enableLODBlending() == true )
        _requireParentTextures = true;

    // A shared registry for tile nodes in the scene graph. Enable revision tracking
    // if requested in the options. Revision tracking lets the registry notify all
    // live tiles of the current map revision so they can inrementally update
    // themselves if necessary.
    _liveTiles = new TileNodeRegistry("live");
    _liveTiles->setMapRevision( _update_mapf->getRevision() );

    if ( _terrainOptions.quickReleaseGLObjects() == true )
    {
        _deadTiles = new TileNodeRegistry("dead");
        _quickReleaseInstalled = false;
        ADJUST_UPDATE_TRAV_COUNT( this, +1 );
    }

    // A shared geometry pool.
    if ( ::getenv("OSGEARTH_REX_NO_POOL") == 0L )
    {
        _geometryPool = new GeometryPool( _terrainOptions );
    }

    // Make a tile loader
    _loader = new PagerLoader( getUID() );
    //_loader = new SimpleLoader();
    this->addChild( _loader.get() );
    
    // handle an already-established map profile:
    MapInfo mapInfo( map );
    if ( _update_mapf->getProfile() )
    {
        // NOTE: this will initialize the map with the startup layers
        onMapInfoEstablished( mapInfo );
    }

    // install a layer callback for processing further map actions:
    map->addMapCallback( new RexTerrainEngineNodeMapCallbackProxy(this) );

    // Prime with existing layers:
    _batchUpdateInProgress = true;

    ElevationLayerVector elevationLayers;
    map->getElevationLayers( elevationLayers );
    for( ElevationLayerVector::const_iterator i = elevationLayers.begin(); i != elevationLayers.end(); ++i )
        addElevationLayer( i->get() );

    ImageLayerVector imageLayers;
    map->getImageLayers( imageLayers );
    for( ImageLayerVector::iterator i = imageLayers.begin(); i != imageLayers.end(); ++i )
        addImageLayer( i->get() );

    _batchUpdateInProgress = false;

    // install some terrain-wide uniforms
    this->getOrCreateStateSet()->getOrCreateUniform(
        "oe_min_tile_range_factor",
        osg::Uniform::FLOAT)->set( *_terrainOptions.minTileRangeFactor() );

    this->getOrCreateStateSet()->getOrCreateUniform(
        "oe_lodblend_delay",
        osg::Uniform::FLOAT)->set( *_terrainOptions.lodBlendDelay() );

    this->getOrCreateStateSet()->getOrCreateUniform(
        "oe_lodblend_duration",
        osg::Uniform::FLOAT)->set( *_terrainOptions.lodBlendDuration() );

    // set up the initial shaders
    updateState();

    // register this instance to the osgDB plugin can find it.
    registerEngine( this );

    // now that we have a map, set up to recompute the bounds
    dirtyBound();
}


osg::BoundingSphere
RexTerrainEngineNode::computeBound() const
{
    //if ( _terrain && _terrain->getNumChildren() > 0 )
    //{
    //    return _terrain->getBound();
    //}
    //else
    {
        return TerrainEngineNode::computeBound();
    }
}

void
RexTerrainEngineNode::invalidateRegion(const GeoExtent& extent,
                                       unsigned         minLevel,
                                       unsigned         maxLevel)
{
    if ( _liveTiles.valid() )
    {
        GeoExtent extentLocal = extent;

        if ( !extent.getSRS()->isEquivalentTo(this->getMap()->getSRS()) )
        {
            extent.transform(this->getMap()->getSRS(), extentLocal);
        }
        
        _liveTiles->setDirty(extentLocal, minLevel, maxLevel);
    }
}

void
RexTerrainEngineNode::refresh(bool forceDirty)
{
    if ( _batchUpdateInProgress )
    {
        _refreshRequired = true;
    }
    else
    {
        dirtyTerrain();

        _refreshRequired = false;
    }
}

void
RexTerrainEngineNode::onMapInfoEstablished( const MapInfo& mapInfo )
{
    dirtyTerrain();
}

osg::StateSet*
RexTerrainEngineNode::getSurfaceStateSet()
{
#ifdef USE_RENDER_BINS
    return _surfaceRenderBinPrototype->getStateSet();
#else
    return _terrain ? _terrain->getOrCreateStateSet() : 0L;
#endif
}

osg::StateSet*
RexTerrainEngineNode::getLandCoverStateSet()
{
#ifdef USE_RENDER_BINS
    return _landCoverRenderBinPrototype->getStateSet();
#else
    return _terrain ? _terrain->getOrCreateStateSet() : 0L;
#endif
}


void
RexTerrainEngineNode::setupRenderBindings()
{
    _renderBindings.push_back( SamplerBinding() );
    SamplerBinding& color = _renderBindings.back();
    color.usage()       = SamplerBinding::COLOR;
    color.samplerName() = "oe_layer_tex";
    color.matrixName()  = "oe_layer_texMatrix";
    this->getResources()->reserveTextureImageUnit( color.unit(), "Terrain Color" );

    _renderBindings.push_back( SamplerBinding() );
    SamplerBinding& elevation = _renderBindings.back();
    elevation.usage()       = SamplerBinding::ELEVATION;
    elevation.samplerName() = "oe_tile_elevationTex";
    elevation.matrixName()  = "oe_tile_elevationTexMatrix";
    this->getResources()->reserveTextureImageUnit( elevation.unit(), "Terrain Elevation" );

    _renderBindings.push_back( SamplerBinding() );
    SamplerBinding& normal = _renderBindings.back();
    normal.usage()       = SamplerBinding::NORMAL;
    normal.samplerName() = "oe_tile_normalTex";
    normal.matrixName()  = "oe_tile_normalTexMatrix";
    this->getResources()->reserveTextureImageUnit( normal.unit(), "Terrain Normals" );
}

void
RexTerrainEngineNode::dirtyTerrain()
{
    //TODO: scrub the geometry pool?

#if 0
    if ( !_surfaceGroup.valid() )
    {
        _surfaceGroup = new osg::Group();
        this->addChild( _surfaceGroup.get() );
    }

    if ( !_landCoverGroup.valid() )
    {
        _landCoverGroup = new osg::Group();
        this->addChild( _landCoverGroup.get() );
    }

    if ( _terrain.valid() )
    {
        _surfaceGroup->removeChild( _terrain.get() );
        _landCoverGroup->removeChild( _terrain.get() );
        _terrain = 0L;
    }

    _terrain = new osg::Group();

    _surfaceGroup->addChild( _terrain.get() );
    _landCoverGroup->addChild( _terrain.get() );
#else
    if ( _terrain )
    {
        this->removeChild( _terrain );
    }

    // New terrain
    _terrain = new osg::Group();
    this->addChild( _terrain );
#endif


#ifdef USE_RENDER_BINS
    //_terrain->getOrCreateStateSet()->setRenderBinDetails( 0, _surfaceRenderBinPrototype->getName() );
    //_terrain->getOrCreateStateSet()->setNestRenderBins(false);
#else
    _terrain->getOrCreateStateSet()->setRenderBinDetails(0, "SORT_FRONT_TO_BACK");
#endif

    // are we LOD blending?
    bool setupParentData = 
        _terrainOptions.enableLODBlending() == true ||
        this->parentTexturesRequired();
    
    // reserve GPU unit for the main color texture:
    if ( _renderBindings.empty() )
    {
        setupRenderBindings();
    }

    // Factory to create the root keys:
    EngineContext* context = getEngineContext();

    // Build the first level of the terrain.
    // Collect the tile keys comprising the root tiles of the terrain.
    std::vector<TileKey> keys;
    _update_mapf->getProfile()->getAllKeysAtLOD( *_terrainOptions.firstLOD(), keys );

    // create a root node for each root tile key.
    OE_INFO << LC << "Creating " << keys.size() << " root keys.." << std::endl;

    //TilePagedLOD* root = new TilePagedLOD( _uid, _liveTiles, _deadTiles );
    //_terrain->addChild( root );

    osg::ref_ptr<osgDB::Options> dbOptions = Registry::instance()->cloneOrCreateOptions();

    unsigned child = 0;
    for( unsigned i=0; i<keys.size(); ++i )
    {
        TileNode* tileNode = new TileNode();
                
        // Next, build the surface geometry for the node.
        tileNode->create( keys[i], context );

        _terrain->addChild( tileNode );
    }

    updateState();

    // Call the base class
    TerrainEngineNode::dirtyTerrain();
}

namespace
{
    // debugging
    struct CheckForOrphans : public TileNodeRegistry::ConstOperation {
        void operator()( const TileNodeRegistry::TileNodeMap& tiles ) const {
            unsigned count = 0;
            for(TileNodeRegistry::TileNodeMap::const_iterator i = tiles.begin(); i != tiles.end(); ++i ) {
                if ( i->second->referenceCount() == 1 ) {
                    count++;
                }
            }
            if ( count > 0 )
                OE_WARN << LC << "Oh no! " << count << " orphaned tiles in the reg" << std::endl;
        }
    };
}


void
RexTerrainEngineNode::traverse(osg::NodeVisitor& nv)
{
    if ( nv.getVisitorType() == nv.UPDATE_VISITOR && _quickReleaseInstalled == false )
    {
        osg::Camera* cam = findFirstParentOfType<osg::Camera>( this );
        if ( cam )
        {
            // get the installed PDC so we can nest them:
            osg::Camera::DrawCallback* cbToNest = cam->getPostDrawCallback();

            // if it's another QR callback, we'll just replace it.
            QuickReleaseGLObjects* previousQR = dynamic_cast<QuickReleaseGLObjects*>(cbToNest);
            if ( previousQR )
                cbToNest = previousQR->_next.get();

            cam->setPostDrawCallback( new QuickReleaseGLObjects(_deadTiles.get(), cbToNest) );

            _quickReleaseInstalled = true;
            OE_INFO << LC << "Quick release enabled" << std::endl;

            // knock down the trav count set in the constructor.
            ADJUST_UPDATE_TRAV_COUNT( this, -1 );
        }
    }

    if ( nv.getVisitorType() == nv.CULL_VISITOR )
    {
        // Inform the registry of the current frame so that Tiles have access
        // to the information.
        if ( _liveTiles.valid() && nv.getFrameStamp() )
        {
            _liveTiles->setTraversalFrame( nv.getFrameStamp()->getFrameNumber() );
        }
    }

#if 0
    static int c = 0;
    if ( ++c % 60 == 0 )
    {
        OE_NOTICE << LC << "Live = " << _liveTiles->size() << ", Dead = " << _deadTiles->size() << std::endl;
        _liveTiles->run( CheckForOrphans() );
    }
#endif
    
    if ( _loader.valid() ) // ensures that postInitialize has run
    {
        // Pass the tile creation context to the traversal.
        osg::ref_ptr<osg::Referenced> data = nv.getUserData();    
        nv.setUserData( this->getEngineContext() );

        TerrainEngineNode::traverse( nv );

        if ( data.valid() )
            nv.setUserData( data.get() );
    }

    else
    {
        TerrainEngineNode::traverse( nv );
    }
}


EngineContext*
RexTerrainEngineNode::getEngineContext()
{
    osg::ref_ptr<EngineContext>& factory = _perThreadTileGroupFactories.get(); // thread-safe get
    if ( !factory.valid() )
    {
        // create a compiler for compiling tile models into geometry
        // TODO: pass this somehow...?
        bool optimizeTriangleOrientation = 
            getMap()->getMapOptions().elevationInterpolation() != INTERP_TRIANGULATE;

        // initialize a key node factory.
        factory = new EngineContext(
            getMap(),
            this, // engine
            _geometryPool.get(),
            _loader.get(),
            _liveTiles.get(),
            _deadTiles.get(),
            _renderBindings,
            _terrainOptions );
    }

    return factory.get();
}


// no longer used.
osg::Node*
RexTerrainEngineNode::createTile( const TileKey& key )
{
    // TODO: implement again.
    OE_WARN << LC << "createTile is not implemented.\n";
    return 0L;
}


void
RexTerrainEngineNode::onMapModelChanged( const MapModelChange& change )
{
    if ( change.getAction() == MapModelChange::BEGIN_BATCH_UPDATE )
    {
        _batchUpdateInProgress = true;
    }

    else if ( change.getAction() == MapModelChange::END_BATCH_UPDATE )
    {
        _batchUpdateInProgress = false;

        if ( _refreshRequired )
            refresh();

        if ( _stateUpdateRequired )
            updateState();
    }

    else
    {
        // update the thread-safe map model copy:
        if ( _update_mapf->sync() )
        {
            _liveTiles->setMapRevision( _update_mapf->getRevision() );
        }

        // dispatch the change handler
        if ( change.getLayer() )
        {
            // then apply the actual change:
            switch( change.getAction() )
            {
            case MapModelChange::ADD_IMAGE_LAYER:
                addImageLayer( change.getImageLayer() );
                break;
            case MapModelChange::REMOVE_IMAGE_LAYER:
                removeImageLayer( change.getImageLayer() );
                break;
            case MapModelChange::ADD_ELEVATION_LAYER:
                addElevationLayer( change.getElevationLayer() );
                break;
            case MapModelChange::REMOVE_ELEVATION_LAYER:
                removeElevationLayer( change.getElevationLayer() );
                break;
            case MapModelChange::MOVE_IMAGE_LAYER:
                moveImageLayer( change.getFirstIndex(), change.getSecondIndex() );
                break;
            case MapModelChange::MOVE_ELEVATION_LAYER:
                moveElevationLayer( change.getFirstIndex(), change.getSecondIndex() );
                break;
            case MapModelChange::TOGGLE_ELEVATION_LAYER:
                toggleElevationLayer( change.getElevationLayer() );
                break;
            case MapModelChange::ADD_MODEL_LAYER:
            case MapModelChange::REMOVE_MODEL_LAYER:
            case MapModelChange::MOVE_MODEL_LAYER:
            default: 
                break;
            }
        }
    }
}


void
RexTerrainEngineNode::addImageLayer( ImageLayer* layerAdded )
{
    if ( layerAdded && layerAdded->getEnabled() )
    {
        // for a shared layer, allocate a shared image unit if necessary.
        if ( layerAdded->isShared() )
        {
            optional<int>& unit = layerAdded->shareImageUnit();
            if ( !unit.isSet() )
            {
                int temp;
                if ( getResources()->reserveTextureImageUnit(temp) )
                {
                    layerAdded->shareImageUnit() = temp;
                    OE_INFO << LC << "Image unit " << temp << " assigned to shared layer " << layerAdded->getName() << std::endl;
                }
                else
                {
                    OE_WARN << LC << "Insufficient GPU image units to share layer " << layerAdded->getName() << std::endl;
                }
            }

            // Build a sampler binding for the layer.
            if ( unit.isSet() )
            {
                _renderBindings.push_back( SamplerBinding() );
                SamplerBinding& binding = _renderBindings.back();

                binding.sourceUID() = layerAdded->getUID();
                binding.unit()      = unit.get();

                if ( layerAdded->shareTexUniformName().isSet() )
                    binding.samplerName() = layerAdded->shareTexUniformName().get();
                else
                    binding.samplerName() = Stringify() << "oe_layer_" << layerAdded->getUID() << "_tex";

                if ( layerAdded->shareTexMatUniformName().isSet() )
                    binding.matrixName() = layerAdded->shareTexMatUniformName().get();
                else
                    binding.matrixName() = Stringify() << "oe_layer_ " << layerAdded->getUID() << "_texMatrix";
            }
        }
    }

    refresh();
}


void
RexTerrainEngineNode::removeImageLayer( ImageLayer* layerRemoved )
{
    if ( layerRemoved )
    {
        // for a shared layer, release the shared image unit.
        if ( layerRemoved->getEnabled() && layerRemoved->isShared() )
        {
            if ( layerRemoved->shareImageUnit().isSet() )
            {
                getResources()->releaseTextureImageUnit( *layerRemoved->shareImageUnit() );
                layerRemoved->shareImageUnit().unset();
            }

            //TODO: remove the sampler/matrix uniforms
        }
    }

    refresh();
}

void
RexTerrainEngineNode::moveImageLayer( unsigned int oldIndex, unsigned int newIndex )
{
    updateState();
}

void
RexTerrainEngineNode::addElevationLayer( ElevationLayer* layer )
{
    if ( layer == 0L || layer->getEnabled() == false )
        return;

    layer->addCallback( _elevationCallback.get() );

    refresh();
}

void
RexTerrainEngineNode::removeElevationLayer( ElevationLayer* layerRemoved )
{
    if ( layerRemoved->getEnabled() == false )
        return;

    layerRemoved->removeCallback( _elevationCallback.get() );

    refresh();
}

void
RexTerrainEngineNode::moveElevationLayer( unsigned int oldIndex, unsigned int newIndex )
{
    refresh();
}

void
RexTerrainEngineNode::toggleElevationLayer( ElevationLayer* layer )
{
    refresh();
}

// Generates the main shader code for rendering the terrain.
void
RexTerrainEngineNode::updateState()
{
    if ( _batchUpdateInProgress )
    {
        _stateUpdateRequired = true;
    }
    else
    {
        osg::StateSet* terrainStateSet   = _terrain->getOrCreateStateSet();   // everything
        osg::StateSet* surfaceStateSet   = getSurfaceStateSet();    // just the surface
        osg::StateSet* landCoverStateSet = getLandCoverStateSet();  // just the land cover
        
        // required for multipass tile rendering to work
        surfaceStateSet->setAttributeAndModes(
            new osg::Depth(osg::Depth::LEQUAL, 0, 1, true) );

        // activate standard mix blending.
        terrainStateSet->setAttributeAndModes( 
            new osg::BlendFunc(GL_SRC_ALPHA,GL_ONE_MINUS_SRC_ALPHA),
            osg::StateAttribute::ON );

        // install patch param if we are tessellation on the GPU.
        if ( _terrainOptions.gpuTessellation() == true )
        {
            terrainStateSet->setAttributeAndModes( new osg::PatchParameter(3) );
        }

        // install shaders, if we're using them.
        if ( Registry::capabilities().supportsGLSL() )
        {
            VirtualProgram* terrainVP = VirtualProgram::getOrCreate(terrainStateSet);
            terrainVP->setName( "Rex Terrain" );
            
            Shaders package;
            
            bool useTerrainColor = _terrainOptions.color().isSet();
            package.define("OE_REX_USE_TERRAIN_COLOR", useTerrainColor);
            if ( useTerrainColor )
            {
                surfaceStateSet->addUniform(new osg::Uniform("oe_terrain_color", _terrainOptions.color().get()));
            }

            bool useBlending = _terrainOptions.enableBlending().get();
            package.define("OE_REX_USE_BLENDING", useBlending);

            // Funtions that affect only the terrain surface:
            VirtualProgram* surfaceVP = VirtualProgram::getOrCreate(surfaceStateSet);
            surfaceVP->setName("Rex Surface");

            // Functions that affect the entire terrain:         
            package.loadFunction(surfaceVP, package.VERT_MODEL);
            package.loadFunction(surfaceVP, package.VERT_VIEW);
            package.loadFunction(surfaceVP, package.FRAG);

            if ( landCoverStateSet )
            {
                VirtualProgram* landCoverVP = VirtualProgram::getOrCreate(landCoverStateSet);
                package.loadFunction(landCoverVP, package.VERT_MODEL);

                // enable alpha-to-coverage multisampling for vegetation.
                landCoverStateSet->setMode(GL_SAMPLE_ALPHA_TO_COVERAGE_ARB, 1);

                // uniform that communicates the availability of multisampling.
                landCoverStateSet->addUniform( new osg::Uniform(
                    "oe_terrain_hasMultiSamples",
                    osg::DisplaySettings::instance()->getMultiSamples()) );

                landCoverStateSet->setAttributeAndModes(
                    new osg::BlendFunc(GL_ONE, GL_ZERO, GL_ONE, GL_ZERO),
                    osg::StateAttribute::OVERRIDE );
            }

            // assemble color filter code snippets.
            bool haveColorFilters = false;
            {
                // Color filter frag function:
                std::string fs_colorfilters =
                    "#version " GLSL_VERSION_STR "\n"
                    GLSL_DEFAULT_PRECISION_FLOAT "\n"
                    "uniform int oe_layer_uid; \n"
                    "$COLOR_FILTER_HEAD"
                    "void oe_rexEngine_applyFilters(inout vec4 color) \n"
                    "{ \n"
                        "$COLOR_FILTER_BODY"
                    "} \n";

                std::stringstream cf_head;
                std::stringstream cf_body;
                const char* I = "    ";

                // second, install the per-layer color filter functions AND shared layer bindings.
                bool ifStarted = false;
                int numImageLayers = _update_mapf->imageLayers().size();
                for( int i=0; i<numImageLayers; ++i )
                {
                    ImageLayer* layer = _update_mapf->getImageLayerAt(i);
                    if ( layer->getEnabled() )
                    {
                        // install Color Filter function calls:
                        const ColorFilterChain& chain = layer->getColorFilters();
                        if ( chain.size() > 0 )
                        {
                            haveColorFilters = true;
                            if ( ifStarted ) cf_body << I << "else if ";
                            else             cf_body << I << "if ";
                            cf_body << "(oe_layer_uid == " << layer->getUID() << ") {\n";
                            for( ColorFilterChain::const_iterator j = chain.begin(); j != chain.end(); ++j )
                            {
                                const ColorFilter* filter = j->get();
                                cf_head << "void " << filter->getEntryPointFunctionName() << "(inout vec4 color);\n";
                                cf_body << I << I << filter->getEntryPointFunctionName() << "(color);\n";
                                filter->install( surfaceStateSet );
                            }
                            cf_body << I << "}\n";
                            ifStarted = true;
                        }
                    }
                }

                if ( haveColorFilters )
                {
                    std::string cf_head_str, cf_body_str;
                    cf_head_str = cf_head.str();
                    cf_body_str = cf_body.str();

                    replaceIn( fs_colorfilters, "$COLOR_FILTER_HEAD", cf_head_str );
                    replaceIn( fs_colorfilters, "$COLOR_FILTER_BODY", cf_body_str );

                    surfaceVP->setFunction(
                        "oe_rexEngine_applyFilters",
                        fs_colorfilters,
                        ShaderComp::LOCATION_FRAGMENT_COLORING,
                        0.0 );
                }
            }

            // Apply uniforms for sampler bindings:
            for(RenderBindings::const_iterator b = _renderBindings.begin(); b != _renderBindings.end(); ++b)
            {
                if ( b->isActive() )
                {
                    terrainStateSet->addUniform( new osg::Uniform(b->samplerName().c_str(), b->unit()) );
                }
            }

            // uniform that controls per-layer opacity
            terrainStateSet->addUniform( new osg::Uniform("oe_layer_opacity", 1.0f) );

            // uniform that conveys the layer UID to the shaders; necessary
            // for per-layer branching (like color filters)
            // UID -1 => no image layer (no texture)
            terrainStateSet->addUniform( new osg::Uniform("oe_layer_uid", (int)-1 ) );

            // uniform that conveys the render order, since the shaders
            // need to know which is the first layer in order to blend properly
            terrainStateSet->addUniform( new osg::Uniform("oe_layer_order", (int)0) );

            // default min/max range uniforms. (max < min means ranges are disabled)
            terrainStateSet->addUniform( new osg::Uniform("oe_layer_minRange", 0.0f) );
            terrainStateSet->addUniform( new osg::Uniform("oe_layer_maxRange", -1.0f) );
            
            terrainStateSet->getOrCreateUniform(
                "oe_min_tile_range_factor",
                osg::Uniform::FLOAT)->set( *_terrainOptions.minTileRangeFactor() );

            // special object ID that denotes the terrain surface.
            terrainStateSet->addUniform( new osg::Uniform(
                Registry::objectIndex()->getObjectIDUniformName().c_str(), OSGEARTH_OBJECTID_TERRAIN) );

        }

        _stateUpdateRequired = false;
    }
}