
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
assert.eq(coll.count(), 12, 'test data (Polygons) insert failed');

