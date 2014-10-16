//
//  this test is related to the Big Polygon (multi-hemisphere)
//  GEO query feature in mongod
//  see:  https://jira.mongodb.org/browse/CAP-1099
//  and:  https://jira.mongodb.org/browse/SERVER-14510
//
// section 4.1.1 setup
//
coll.getMongo().getDB("admin").runCommand({ setParameter : 1, help: true})
coll.getMongo().getDB("admin").runCommand({ setParameter : 1, logLevel: 1})

var coll = db.big_geodata;
coll.drop();

coll.ensureIndex({geo: "2dsphere" });

// point locations are longitude, lattitude
// points in GeoJson format
var pt = {};
pt.geo = {};
pt.name = 'boat ramp';
pt.geo.type = 'Point';
pt.geo.coordinates = [ -97.927117, 30.327376 ];
coll.insert(pt);

pt.name = 'on equator';
pt.geo.coordinates = [ -97.9 , 0 ];
coll.insert(pt);

pt.name = 'just north of equator';
pt.geo.coordinates = [ -97.9 , 0.1 ];
coll.insert(pt);

pt.name = 'just south of equator';
pt.geo.coordinates = [ -97.9 , -0.1 ];
coll.insert(pt);

pt.name = 'north pole';
pt.geo.coordinates = [ -97.9 , 90.0 ];
coll.insert(pt);

pt.name = 'south pole';
pt.geo.coordinates = [ -97.9 , -90.0 ];
coll.insert(pt);

// sanity check
assert.eq(coll.count(), 6, 'test data (Geo Json points) insert failed');



// short line string
var curve = {};
curve.geo = {};
curve.name = 'short line string: PA, LA, 4corners, ATX, Mansfield, FL, Reston, NYC';
curve.geo.type = 'LineString';
curve.geo.coordinates = [
              [ -122.1611953, 37.4420407 ],
              [ -118.283638, 34.028517 ],
              [ -109.045223, 36.9990835 ],
              [ -97.850404, 30.3921555 ],
              [ -97.904187, 30.395457 ],
              [ -86.600836, 30.398147 ],
              [ -77.357837, 38.9589935 ],
              [ -73.987723, 40.7575074 ]
];
coll.insert(curve);


// greater than 1000 points long line string, south pole to north pole
var lon = 2.349902;
var startLat = -90.0;
var endLat = 90.0;
var latStep = 180.0 / 1024;
curve.name = '1024 point long line string from south pole to north pole';
curve.geo.type = 'LineString';
curve.geo.coordinates = [];
var i = 0;
for (var lat = startLat; lat <= endLat; lat += latStep) {
  curve.geo.coordinates.push( [ lon, lat ] );
}
coll.insert(curve);


// line crossing the equator
curve.name = 'line crossing equator';
curve.geo.type = 'LineString';
curve.geo.coordinates = [
              [ -77.0451853, -12.0553442 ],
              [ -76.7784557, 18.0098528 ]
];
coll.insert(curve);


// sanity check
assert.eq(coll.count(), 9, 'test data (line strings) insert failed');


// Polygon in GeoJson format
var poly = {};
poly.geo = {};
poly.name = 'GeoJson polygon';
poly.geo.type = 'Polygon';
poly.geo.coordinates = [
              [ [ -80.0, 30.0 ], [ -40.0, 30.0 ], [ -40.0, 60.0 ], [-80.0, 60.0 ], [ -80.0, 30.0 ] ]
            ];
coll.insert(poly);



// Polygon w/ hole
var polyWithHole = {};
polyWithHole.geo = {};
polyWithHole.name = 'polygon w/ hole';
polyWithHole.geo.type = 'Polygon';
polyWithHole.geo.coordinates = [
              [ [ -80.0, 30.0 ], [ -40.0, 30.0 ], [ -40.0, 60.0 ], [-80.0, 60.0 ], [ -80.0, 30.0 ] ],
              [ [ -70.0, 40.0 ], [ -60.0, 40.0 ], [ -60.0, 50.0 ], [-70.0, 50.0 ], [ -70.0, 40.0 ] ]
            ];
coll.insert(polyWithHole);


// Polygon w/ holes
var polyWithTwoHoles = {};
polyWithTwoHoles.geo = {};
polyWithTwoHoles.name = 'polygon w/ two holes';
polyWithTwoHoles.geo.type = 'Polygon';
polyWithTwoHoles.geo.coordinates = [
              [ [ -80.0, 30.0 ], [ -40.0, 30.0 ], [ -40.0, 60.0 ], [-80.0, 60.0 ], [ -80.0, 30.0 ] ],
              [ [ -70.0, 40.0 ], [ -60.0, 40.0 ], [ -60.0, 50.0 ], [-70.0, 50.0 ], [ -70.0, 40.0 ] ],
              [ [ -55.0, 40.0 ], [ -45.0, 40.0 ], [ -45.0, 50.0 ], [-55.0, 50.0 ], [ -55.0, 40.0 ] ]
            ];
coll.insert(polyWithTwoHoles);


// sanity check
assert.eq(coll.count(), 12, 'test data (Polygons 1) insert failed');




// Polygon covering north pole
var polyNP = {};
polyNP.geo = {};
polyNP.name = 'polygon covering North pole';
polyNP.geo.type = 'Polygon';
polyNP.geo.coordinates = [
              [ [ -120.0, 89.0 ], [ 0.0, 89.0 ], [ 120.0, 89.0 ], [ -120.0, 89.0 ] ]
            ];
coll.insert(polyNP);

// Polygon covering south pole
var polySP = {};
polySP.geo = {};
polySP.name = 'polygon covering South pole';
polySP.geo.type = 'Polygon';
polySP.geo.coordinates = [
              [ [ -120.0, -89.0 ], [ 0.0, -89.0 ], [ 120.0, -89.0 ], [ -120.0, -89.0 ] ]
            ];
coll.insert(polySP);


// proper CRS object
var CRS = {};
CRS.type = 'name';
CRS.properties = {};
CRS.properties.name = 'urn:mongodb:strictwindingcrs:EPSG:4326';




// Big Polygon covering both poles
// this document will NOT be found in the 2.8 release
// b/c this feature does not allow storage of big polygon objects
var bigPoly = {};
bigPoly.geo = {};
bigPoly.name = 'big polygon/rectangle covering both poles';
bigPoly.geo.type = 'Polygon';
bigPoly.geo.crs = CRS;
bigPoly.geo.coordinates = [
              [ [ -130.0, 89.0 ], [ -120.0, 89.0 ], [ -120.0, -89.0 ], [ -130.0, -89.0 ], [ -130.0, 89.0 ] ]
            ];
var result = coll.insert(bigPoly);
// will not work in legacy mode (non-WriteCommand)
assert.eq(0, result.nInserted);


// sanity check
assert.eq(coll.count(), 14, 'test data (Polygons 2) insert failed');



// Polygon w/ hole @ North pole
var polyWithHole = {};
polyWithHole.geo = {};
polyWithHole.name = 'polygon (triangle) w/ hole at North pole';
polyWithHole.geo.type = 'Polygon';
polyWithHole.geo.coordinates = [
              [ [ -120.0, 80.0 ], [ 0.0, 80.0 ], [ 120.0, 80.0 ], [-120.0, 80.0 ] ],
              [ [ -120.0, 88.0 ], [ 0.0, 88.0 ], [ 120.0, 88.0 ], [-120.0, 88.0 ] ]
            ];
coll.insert(polyWithHole);




// Polygon with edge on equator
poly = {};
poly.geo = {};
poly.name = 'polygon with edge on equator';
poly.geo.type = 'Polygon';
poly.geo.coordinates = [
              [ [ -120.0, 0.0 ], [ 120.0, 0.0 ], [ 0.0, 90.0 ], [ -120.0, 0.0 ] ]
            ];
coll.insert(poly);



// Polygon just inside a single hemisphere (Northern)
// China, California, Europe
poly = {};
poly.geo = {};
poly.name = 'polygon just inside Northern hemisphere';
poly.geo.type = 'Polygon';
poly.geo.coordinates = [
              [ [ 120.0, 0.000001 ], [ -120.0, 0.000001 ], [ 0.0, 0.000001 ], [ 120.0, 0.000001 ] ]
            ];
coll.insert(poly);

poly = {};
poly.geo = {};
poly.name = 'polygon inside Northern hemisphere';
poly.geo.type = 'Polygon';
poly.geo.coordinates = [
              [ [ 120.0, 80.0 ], [ -120.0, 80.0 ], [ 0.0, 80.0 ], [ 120.0, 80.0 ] ]
            ];
coll.insert(poly);



// Polygon just inside a single hemisphere (Southern)
// Pacific Ocean South of California, Indonesia, Africa
poly = {};
poly.geo = {};
poly.name = 'polygon just inside Southern hemisphere';
poly.geo.type = 'Polygon';
poly.geo.coordinates = [
              [ [ -120.0, -0.000001 ], [ 120.0, -0.000001 ], [ 0.0, -0.000001 ], [ -120.0, -0.000001 ] ]
            ];
coll.insert(poly);

poly = {};
poly.geo = {};
poly.name = 'polygon inside Southern hemisphere';
poly.geo.type = 'Polygon';
poly.geo.coordinates = [
              [ [ -120.0, -80.0 ], [ 120.0, -80.0 ], [ 0.0, -80.0 ], [ -120.0, -80.0 ] ]
            ];
coll.insert(poly);


// sanity check
assert.eq(coll.count(), 20, 'test data (Polygons 3) insert failed');


// MultiPoint
var multiPoint = {};
multiPoint.geo = {};
multiPoint.name = 'multiple points (MultiPoint): PA, LA, 4corners, ATX, Mansfield, FL, Reston, NYC';
multiPoint.geo.type = 'MultiPoint';
multiPoint.geo.coordinates = [
              [ -122.1611953, 37.4420407 ],
              [ -118.283638, 34.028517 ],
              [ -109.045223, 36.9990835 ],
              [ -97.850404, 30.3921555 ],
              [ -97.904187, 30.395457 ],
              [ -86.600836, 30.398147 ],
              [ -77.357837, 38.9589935 ],
              [ -73.987723, 40.7575074 ]
];
coll.insert(multiPoint);

// MultiPoint w/ single point
multiPoint.name = 'single point (MultiPoint): PA';
multiPoint.geo.type = 'MultiPoint';
multiPoint.geo.coordinates = [
              [ -122.1611953, 37.4420407 ]
];
coll.insert(multiPoint);


// MultiPoint w/ two points in Shenzhen
multiPoint.name = 'two points (MultiPoint): Shenzhen, Guangdong, China';
multiPoint.geo.type = 'MultiPoint';
multiPoint.geo.coordinates = [
              [ 114.0538788, 22.5551603 ],
              [ 114.022837, 22.44395 ]
];
coll.insert(multiPoint);


// MultiPoint w/ two points, one in Shenzhen
multiPoint.name = 'two points (MultiPoint) but only one in: Shenzhen, Guangdong, China';
multiPoint.geo.type = 'MultiPoint';
multiPoint.geo.coordinates = [
              [ 114.0538788, 22.5551603 ],
              [ 113.743858, 23.025815 ]

];
coll.insert(multiPoint);


// sanity check
assert.eq(coll.count(), 24, 'test data (MultiPoint) insert failed');

// MultiLineString
var multiLineString = {};
multiLineString.geo = {};
multiLineString.name = 'multi line string: new zealand bays';
multiLineString.geo.type = 'MultiLineString';
multiLineString.geo.coordinates = [
  [
    [ 172.803869, -43.592789 ],
    [ 172.659335, -43.620348 ],
    [ 172.684038, -43.636528 ],
    [ 172.820922, -43.605325 ]
  ],
  [
    [ 172.830497, -43.607768 ],
    [ 172.813263, -43.656319 ],
    [ 172.823096, -43.660996 ],
    [ 172.850943, -43.607609 ]
  ],
  [
    [ 172.912056, -43.623148 ],
    [ 172.887696, -43.670897 ],
    [ 172.900469, -43.676178 ],
    [ 172.931735, -43.622839 ]
  ]
];
coll.insert(multiLineString);

// sanity check
assert.eq(coll.count(), 25, 'test data (MultiLineString) insert failed');


// MultiPolygon
var multiPolygon = {};
multiPolygon.geo = {};
multiPolygon.name = 'multi polygon: new zealand north and south islands';
multiPolygon.geo.type = 'MultiPolygon';
multiPolygon.geo.coordinates = [
  [
    [
      [ 165.773255, -45.902933 ],
      [ 169.398419, -47.261538 ],
      [ 174.672744, -41.767722 ],
      [ 172.288845, -39.897992 ],
      [ 165.773255, -45.902933 ]
    ]
  ],
  [
    [
      [ 173.166448, -39.778262 ],
      [ 175.342744, -42.677333 ],
      [ 179.913373, -37.224362 ],
      [ 171.475953, -32.688871 ],
      [ 173.166448, -39.778262 ]
    ]
  ]
];
coll.insert(multiPolygon);

// sanity check
assert.eq(coll.count(), 26, 'test data (MultiPolygon) insert failed');


// geometry collection point
var gcPoint = {};
gcPoint.name = 'center of Australia';
gcPoint.type = 'Point';
gcPoint.coordinates = [ 133.985885, -27.240790 ];
 
// geometry collection triangle
var gcTriangle = {};
gcTriangle.name = 'Triangle around Australia';
gcTriangle.type = 'Polygon';
gcTriangle.coordinates = [
              [
                [ 97.423178, -44.735405 ],
                [ 169.845050, -38.432287 ],
                [ 143.824366, 15.966509 ],
                [ 97.423178, -44.735405 ]
              ]
            ];

// GeometryCollection of the two preceedingGeoJSON objects
var geometryCollection = {};
geometryCollection.geo = {};
geometryCollection.name = 'geometry collection: point in Australia and triangle around Australia';
geometryCollection.geo.type = 'GeometryCollection';
geometryCollection.geo.geometries = [
  gcPoint, gcTriangle
];
coll.insert(geometryCollection);

// sanity check
assert.eq(coll.count(), 27, 'test data (GeometryCollection) insert failed');




