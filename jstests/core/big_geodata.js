
var coll = db.big_geodata;
coll.drop();

coll.ensureIndex({ geo : "2dsphere" });

var pt = {}
// point locations are longitude, lattitude
pt.name = 'boat ramp';
pt.loc = [ ];


// points
pt.name = 'on equator';
pt.loc = [ -97.9 , 0 ];
coll.insert(pt);

pt.name = 'just north of equator';
pt.loc = [ -97.9 , 0.1 ];
coll.insert(pt);

pt.name = 'just south of equator';
pt.loc = [ -97.9 , -0.1 ];
coll.insert(pt);

pt.name = 'north pole';
pt.loc = [ -97.9 , 90.0 ];
coll.insert(pt);

pt.name = 'south pole';
pt.loc = [ -97.9 , -90.0 ];
coll.insert(pt);

// sanity check
assert.eq(coll.count(), 5, 'test data (points) insert failed');



// short line string
var curve = {}
curve.name = 'short line string: PA, LA, 4corners, ATX, Mansfield, FL, Reston, NYC';
curve.loc = [ [ -122.1611953, 37.4420407 ],
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
var lon = 2.349902
var startLat = -90.0
var endLat = 90.0
var latStep = 180.0 / 1024
curve.name = '1024 point long line string from south pole to north pole';
curve.loc = [];
var i = 0;
for (var lat = startLat; lat <= endLat; lat += latStep) {
  curve.loc.push( [ lon, lat ] );
}
coll.insert(curve);


// line crossing the equator
curve.name = 'line crossing equator'
curve.loc = [
              [ -77.0451853, -12.0553442 ],
              [ -76.7784557, 18.0098528 ]
            ];

coll.insert(curve);


// sanity check
assert.eq(coll.count(), 8, 'test data (line strings) insert failed');



