
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
              [ -73.987723, 40.7575074 ],
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
coll.insert(bigPoly);

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


