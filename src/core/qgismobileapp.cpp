/***************************************************************************
                            qgismobileapp.cpp
                              -------------------
              begin                : Wed Apr 04 10:48:28 CET 2012
              copyright            : (C) 2012 by Marco Bernasocchi
              email                : marco@bernawebdesign.ch
 ***************************************************************************/

/***************************************************************************
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 ***************************************************************************/

#include <QApplication>

#include <unistd.h>

#include <QStandardPaths>
#include <QtQml/QQmlEngine>
#include <QtQml/QQmlContext>
#include <QtQml/QQmlApplicationEngine>
#include <QtWidgets/QDesktopWidget>
#include <QtWidgets/QFileDialog> // Until native looking QML dialogs are implemented (Qt5.4?)
#include <QtWidgets/QMenu> // Until native looking QML dialogs are implemented (Qt5.4?)
#include <QtWidgets/QMenuBar>
#include <QStandardItemModel>
#include <QPrinter>
#include <QPrintDialog>
#include <QTemporaryFile>

#include <qgslayertreemodel.h>
#include <qgsproject.h>
#include <qgsfeature.h>
#include <qgsvectorlayer.h>
#include <qgssnappingutils.h>
#include <qgsunittypes.h>
#include <qgscoordinatereferencesystem.h>
#include <qgsmapthemecollection.h>
#include <qgsprintlayout.h>
#include <qgslayoutmanager.h>
#include <qgslayoutpagecollection.h>
#include <qgslocator.h>
#include <qgslocatormodel.h>
#include <qgsfield.h>
#include <qgsfieldconstraints.h>

#include "qgsquickmapsettings.h"
#include "qgsquickmapcanvasmap.h"
#include "qgsquickcoordinatetransformer.h"
#include "qgsquickmaptransform.h"

#include "qgismobileapp.h"

#include "appinterface.h"
#include "featurelistmodelselection.h"
#include "featurelistextentcontroller.h"
#include "modelhelper.h"
#include "rubberband.h"
#include "rubberbandmodel.h"
#include "qgsofflineediting.h"
#include "messagelogmodel.h"
#include "attributeformmodel.h"
#include "geometry.h"
#include "featuremodel.h"
#include "layertreemapcanvasbridge.h"
#include "identifytool.h"
#include "submodel.h"
#include "expressionvariablemodel.h"
#include "badlayerhandler.h"
#include "snappingutils.h"
#include "snappingresult.h"
#include "layertreemodel.h"
#include "legendimageprovider.h"
#include "featurelistmodel.h"
#include "qgsrelationmanager.h"
#include "distancearea.h"
#include "printlayoutlistmodel.h"
#include "vertexmodel.h"
#include "maptoscreen.h"
#include "projectsource.h"
#include "locatormodelsuperbridge.h"
#include "qgsgeometrywrapper.h"
#include "linepolygonhighlight.h"
#include "valuemapmodel.h"
#include "referencingfeaturelistmodel.h"
#include "featurechecklistmodel.h"
#include "qgsnetworkaccessmanager.h"
#include "layerloginhandler.h"

QgisMobileapp::QgisMobileapp( QgsApplication *app, QObject *parent )
  : QQmlApplicationEngine( parent )
  , mIface( new AppInterface( this ) )
  , mFirstRenderingFlag( true )
{
#if 0
  QSurfaceFormat format;
  format.setSamples( 8 );
  setFormat( format );
  create();
#endif

  //set the authHandler to qfield-handler
  std::unique_ptr<QgsNetworkAuthenticationHandler> handler;
  mAuthRequestHandler = new QFieldAppAuthRequestHandler();
  handler.reset( mAuthRequestHandler );
  QgsNetworkAccessManager::instance()->setAuthHandler( std::move( handler ) );

  mProject = QgsProject::instance();
  mLayerTree = new LayerTreeModel( mProject->layerTreeRoot(), mProject, this );
  mLegendImageProvider = new LegendImageProvider( mLayerTree->layerTreeModel() );

  initDeclarative();

  load( QUrl( "qrc:/qml/qgismobileapp.qml" ) );

  connect( this, &QQmlApplicationEngine::quit, app, &QgsApplication::quit );

  mMapCanvas = rootObjects().first()->findChild<QgsQuickMapCanvasMap *>();
  mMapCanvas->mapSettings()->setProject( mProject );

  Q_ASSERT_X( mMapCanvas, "QML Init", "QgsQuickMapCanvasMap not found. It is likely that we failed to load the QML files. Check debug output for related messages." );

  connect( mProject, &QgsProject::readProject, this, &QgisMobileapp::onReadProject );

  mLayerTreeCanvasBridge = new LayerTreeMapCanvasBridge( mLayerTree, mMapCanvas->mapSettings(), this );
  connect( this, &QgisMobileapp::loadProjectStarted, mIface, &AppInterface::loadProjectStarted );
  connect( this, &QgisMobileapp::loadProjectEnded, mIface, &AppInterface::loadProjectEnded );
  QTimer::singleShot( 1, this, &QgisMobileapp::onAfterFirstRendering );

  mOfflineEditing = new QgsOfflineEditing();

  mSettings.setValue( "/Map/searchRadiusMM", 5 );
}

void QgisMobileapp::initDeclarative()
{
  // Register QGIS QML types
  qmlRegisterType<QgsSnappingUtils>( "org.qgis", 1, 0, "SnappingUtils" );
  qmlRegisterType<QgsMapLayerProxyModel>( "org.qgis", 1, 0, "MapLayerModel" );
  qmlRegisterType<QgsVectorLayer>( "org.qgis", 1, 0, "VectorLayer" );
  qmlRegisterType<QgsMapThemeCollection>( "org.qgis", 1, 0, "MapThemeCollection" );
  qmlRegisterType<QgsLocatorProxyModel>( "org.qgis", 1, 0, "QgsLocatorProxyModel" );

  qRegisterMetaType<QgsGeometry>( "QgsGeometry" );
  qRegisterMetaType<QgsFeature>( "QgsFeature" );
  qRegisterMetaType<QgsPoint>( "QgsPoint" );
  qRegisterMetaType<QgsPointXY>( "QgsPointXY" );
  qRegisterMetaType<QgsCoordinateTransformContext>( "QgsCoordinateTransformContext" );
  qRegisterMetaType<QgsWkbTypes::GeometryType>( "QgsWkbTypes::GeometryType" ); // could be removed since we have now qmlRegisterUncreatableType<QgsWkbTypes> ?
  qRegisterMetaType<QgsFeatureId>( "QgsFeatureId" );
  qRegisterMetaType<QgsAttributes>( "QgsAttributes" );
  qRegisterMetaType<QgsSnappingConfig>( "QgsSnappingConfig" );
  qRegisterMetaType<QgsUnitTypes::DistanceUnit>( "QgsUnitTypes::DistanceUnit" );
  qRegisterMetaType<QgsUnitTypes::AreaUnit>( "QgsUnitTypes::AreaUnit" );
  qRegisterMetaType<QgsRelation>( "QgsRelation" );
  qRegisterMetaType<QgsField>( "QgsField" );
  qRegisterMetaType<QVariant::Type>( "QVariant::Type" );
  qRegisterMetaType<QgsDefaultValue>( "QgsDefaultValue" );
  qRegisterMetaType<QgsFieldConstraints>( "QgsFieldConstraints" );

  qmlRegisterUncreatableType<QgsProject>( "org.qgis", 1, 0, "Project", "" );
  qmlRegisterUncreatableType<QgsCoordinateReferenceSystem>( "org.qgis", 1, 0, "CoordinateReferenceSystem", "" );
  qmlRegisterUncreatableType<QgsUnitTypes>( "org.qgis", 1, 0, "QgsUnitTypes", "" );
  qmlRegisterUncreatableType<QgsRelationManager>( "org.qgis", 1, 0, "RelationManager", "The relation manager is available from the QgsProject. Try `qgisProject.relationManager`" );
  qmlRegisterUncreatableType<QgsWkbTypes>( "org.qgis", 1, 0, "QgsWkbTypes", "" );
  qmlRegisterUncreatableType<QgsMapLayer>( "org.qgis", 1, 0, "MapLayer", "" );

  // Register QgsQuick QML types
  qmlRegisterType<QgsQuickMapCanvasMap>( "org.qgis", 1, 0, "MapCanvasMap" );
  qmlRegisterType<QgsQuickMapSettings>( "org.qgis", 1, 0, "MapSettings" );
  qmlRegisterType<QgsQuickCoordinateTransformer>( "org.qfield", 1, 0, "CoordinateTransformer" );
  qmlRegisterSingletonType<QgsQuickUtils>( "Utils", 1, 0, "Utils", utilsSingletonProvider );
  qmlRegisterType<QgsQuickMapTransform>( "org.qgis", 1, 0, "MapTransform" );

  // Register QField QML types
  qmlRegisterType<MultiFeatureListModel>( "org.qgis", 1, 0, "MultiFeatureListModel" );
  qmlRegisterType<FeatureListModel>( "org.qgis", 1, 0, "FeatureListModel" );
  qmlRegisterType<FeatureListModelSelection>( "org.qgis", 1, 0, "FeatureListModelSelection" );
  qmlRegisterType<FeatureListExtentController>( "org.qgis", 1, 0, "FeaturelistExtentController" );
  qmlRegisterType<Geometry>( "org.qgis", 1, 0, "Geometry" );
  qmlRegisterType<ModelHelper>( "org.qgis", 1, 0, "ModelHelper" );
  qmlRegisterType<Rubberband>( "org.qgis", 1, 0, "Rubberband" );
  qmlRegisterType<RubberbandModel>( "org.qgis", 1, 0, "RubberbandModel" );
  qmlRegisterType<PictureSource>( "org.qgis", 1, 0, "PictureSource" );
  qmlRegisterType<ProjectSource>( "org.qgis", 1, 0, "ProjectSource" );
  qmlRegisterType<MessageLogModel>( "org.qgis", 1, 0, "MessageLogModel" );
  qmlRegisterType<AttributeFormModel>( "org.qfield", 1, 0, "AttributeFormModel" );
  qmlRegisterType<FeatureModel>( "org.qfield", 1, 0, "FeatureModel" );
  qmlRegisterType<IdentifyTool>( "org.qfield", 1, 0, "IdentifyTool" );
  qmlRegisterType<SubModel>( "org.qfield", 1, 0, "SubModel" );
  qmlRegisterType<ExpressionVariableModel>( "org.qfield", 1, 0, "ExpressionVariableModel" );
  qmlRegisterType<BadLayerHandler>( "org.qfield", 1, 0, "BadLayerHandler" );
  qmlRegisterType<SnappingUtils>( "org.qfield", 1, 0, "SnappingUtils" );
  qmlRegisterType<DistanceArea>( "org.qfield", 1, 0, "DistanceArea" );
  qmlRegisterType<FocusStack>( "org.qfield", 1, 0, "FocusStack" );
  qmlRegisterType<PrintLayoutListModel>( "org.qfield", 1, 0, "PrintLayoutListModel" );
  qmlRegisterType<VertexModel>( "org.qfield", 1, 0, "VertexModel" );
  qmlRegisterType<MapToScreen>( "org.qfield", 1, 0, "MapToScreen" );
  qmlRegisterType<LocatorModelSuperBridge>( "org.qfield", 1, 0, "LocatorModelSuperBridge" );
  qmlRegisterType<LocatorActionsModel>( "org.qfield", 1, 0, "LocatorActionsModel" );
  qmlRegisterType<LinePolygonHighlight>( "org.qfield", 1, 0, "LinePolygonHighlight" );
  qmlRegisterType<QgsGeometryWrapper>( "org.qfield", 1, 0, "QgsGeometryWrapper" );
  qmlRegisterType<ValueMapModel>( "org.qfield", 1, 0, "ValueMapModel" );
  qmlRegisterType<ReferencingFeatureListModel>( "org.qgis", 1, 0, "ReferencingFeatureListModel" );
  qmlRegisterType<FeatureCheckListModel>( "org.qgis", 1, 0, "FeatureCheckListModel" );

  qmlRegisterUncreatableType<AppInterface>( "org.qgis", 1, 0, "QgisInterface", "QgisInterface is only provided by the environment and cannot be created ad-hoc" );
  qmlRegisterUncreatableType<Settings>( "org.qgis", 1, 0, "Settings", "" );
  qmlRegisterUncreatableType<PlatformUtilities>( "org.qgis", 1, 0, "PlatformUtilities", "" );
  qmlRegisterUncreatableType<LayerTreeModel>( "org.qfield", 1, 0, "LayerTreeModel", "The LayerTreeModel is available as context property `layerTree`." );

  qRegisterMetaType<SnappingResult>( "SnappingResult" );

  // Calculate device pixels
  int dpiX = QApplication::desktop()->physicalDpiX();
  int dpiY = QApplication::desktop()->physicalDpiY();
  int dpi = dpiX < dpiY ? dpiX : dpiY; // In case of asymetrical DPI. Improbable
  float dp = dpi * 0.00768443;

  // Register some globally available variables
  rootContext()->setContextProperty( "dp", dp );
  rootContext()->setContextProperty( "qgisProject", mProject );
  rootContext()->setContextProperty( "iface", mIface );
  rootContext()->setContextProperty( "settings", &mSettings );
  rootContext()->setContextProperty( "version", QString( "" VERSTR ) );
  rootContext()->setContextProperty( "versionCode", QString( "" VERSIONCODE ) );
  rootContext()->setContextProperty( "layerTree", mLayerTree );
  rootContext()->setContextProperty( "platformUtilities", &mPlatformUtils );
  rootContext()->setContextProperty( "CrsFactory", QVariant::fromValue<QgsCoordinateReferenceSystem>( mCrsFactory ) );
  rootContext()->setContextProperty( "UnitTypes", QVariant::fromValue<QgsUnitTypes>( mUnitTypes ) );
  rootContext()->setContextProperty( "LocatorModelNoGroup", QgsLocatorModel::NoGroup );
  rootContext()->setContextProperty( "qfieldAuthRequestHandler", mAuthRequestHandler );

  addImageProvider( QLatin1String( "legend" ), mLegendImageProvider );
}

void QgisMobileapp::loadProjectQuirks()
{
  // force update of canvas, without automatic changes to extent and OTF projections
  bool autoEnableCrsTransform = mLayerTreeCanvasBridge->autoEnableCrsTransform();
  bool autoSetupOnFirstLayer = mLayerTreeCanvasBridge->autoSetupOnFirstLayer();
  mLayerTreeCanvasBridge->setAutoEnableCrsTransform( false );
  mLayerTreeCanvasBridge->setAutoSetupOnFirstLayer( false );

  mLayerTreeCanvasBridge->setCanvasLayers();

  if ( autoEnableCrsTransform )
    mLayerTreeCanvasBridge->setAutoEnableCrsTransform( true );

  if ( autoSetupOnFirstLayer )
    mLayerTreeCanvasBridge->setAutoSetupOnFirstLayer( true );
}

void QgisMobileapp::onReadProject( const QDomDocument &doc )
{
  Q_UNUSED( doc );
  QMap<QgsVectorLayer *, QgsFeatureRequest> requests;
  QSettings().setValue( "/qgis/project/lastProjectFile", mProject->fileName() );
  const QList<QgsMapLayer *> mapLayers { mProject->mapLayers().values() };
  for ( QgsMapLayer *layer : mapLayers )
  {
    QgsVectorLayer *vl = qobject_cast<QgsVectorLayer *>( layer );
    if ( vl )
    {
      const QVariant itinerary = vl->customProperty( "qgisMobile/itinerary" );
      if ( itinerary.isValid() )
      {
        requests.insert( vl, QgsFeatureRequest().setFilterExpression( itinerary.toString() ) );
      }
    }
  }
  if ( requests.count() )
  {
    qDebug() << QString( "Loading itinerary for %1 layers." ).arg( requests.count() );
    mIface->openFeatureForm();
  }
}

void QgisMobileapp::onAfterFirstRendering()
{
  // This should get triggered exactly once, so we disconnect it right away
  // disconnect( this, &QgisMobileapp::afterRendering, this, &QgisMobileapp::onAfterFirstRendering );

  if ( mFirstRenderingFlag )
  {
    if ( qApp->arguments().count() > 1 )
    {
      loadProjectFile( qApp->arguments().last() );
    }
    else if ( !mPlatformUtils.qgsProject().isNull() )
    {
      loadProjectFile( mPlatformUtils.qgsProject() );
    }
    else
    {
      QTimer::singleShot( 0, this, SLOT( loadLastProject() ) );
    }
    mFirstRenderingFlag = false;
  }
}

void QgisMobileapp::loadLastProject()
{
  QVariant lastProjectFile = QSettings().value( "/qgis/project/lastProjectFile" );
  if ( lastProjectFile.isValid() )
    loadProjectFile( lastProjectFile.toString() );
}

void QgisMobileapp::loadProjectFile( const QString &path )
{
  mAuthRequestHandler->resetCanceledRealms();
  reloadProjectFile( path );
}

void QgisMobileapp::reloadProjectFile( const QString &path )
{
  mProject->removeAllMapLayers();
  emit loadProjectStarted( path );
  mProject->read( path );
  loadProjectQuirks();

  emit loadProjectEnded();
}

void QgisMobileapp::print( int layoutIndex )
{
  const QList<QgsPrintLayout *> projectLayouts( mProject->layoutManager()->printLayouts() );

  const auto &layoutToPrint = projectLayouts.at( layoutIndex );

  if ( layoutToPrint->pageCollection()->pageCount() == 0 )
    return;

  QPrinter printer;
  QString documentsLocation = QStringLiteral( "%1/QField" ).arg( QStandardPaths::writableLocation( QStandardPaths::DocumentsLocation ) );
  QDir documentsDir( documentsLocation );
  if ( !documentsDir.exists() )
    documentsDir.mkpath( "." );

  printer.setOutputFileName( documentsLocation  + '/' + layoutToPrint->name() + QStringLiteral( ".pdf" ) );

  QgsLayoutExporter::PrintExportSettings printSettings;
  printSettings.rasterizeWholeImage = layoutToPrint->customProperty( QStringLiteral( "rasterize" ), false ).toBool();

  layoutToPrint->refresh();

  QgsLayoutExporter exporter = QgsLayoutExporter( layoutToPrint );
  exporter.print( printer, printSettings );

  mPlatformUtils.open( QStringLiteral( "file:///" ) + printer.outputFileName(), QStringLiteral( "application/pdf" ) );
}

bool QgisMobileapp::event( QEvent *event )
{
  if ( event->type() == QEvent::Close )
    QgsApplication::instance()->quit();

  return QQmlApplicationEngine::event( event );
}

QgisMobileapp::~QgisMobileapp()
{
  delete mOfflineEditing;
  mProject->removeAllMapLayers();
  // Reintroduce when created on the heap
  delete mProject;
}
