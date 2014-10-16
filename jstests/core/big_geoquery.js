//
//  this test is related to the Big Polygon (multi-hemisphere)
//  GEO query feature in mongod
//  see:  https://jira.mongodb.org/browse/CAP-1099
//  and:  https://jira.mongodb.org/browse/SERVER-14510
//

//coll.getMongo().getDB("admin").runCommand({ setParameter : 1, help: true})
coll.getMongo().getDB("admin").runCommand({ setParameter : 1, logLevel: 1})

var coll = db.big_geodata;


// Triangle around Shenzhen, China
var shenzhenPoly = {};
shenzhenPoly.type = 'Polygon';
shenzhenPoly.coordinates = [
                             [
                               [ 114.0834046, 22.6648202 ],
                               [ 113.8293457, 22.3819359 ],
                               [ 114.2736054, 22.4047911 ],
                               [ 114.0834046, 22.6648202 ]
                             ]
                           ];


// expect one doc, MultiPoint with two points
// inside Shenzhen triangle
var curs = coll.find({geo: {$geoWithin: {$geometry: shenzhenPoly}}});
assert.eq(1, curs.count(), 'expected only one doc within shenzhen triangle');

// expect three docs intersecting with Shenzhen triangle:
//   northern hemisphere poly
//   Two point MultiPoint with two points in triangle
//   Two point MultiPoint with one point in triangle
var curs = coll.find({geo: {$geoIntersects: {$geometry: shenzhenPoly}}});
assert.eq(3, curs.count(), 'expected three docs intersecting with shenzhen triangle');


var CRS = {};
CRS.type = 'name';
CRS.properties = {};

// this CRS string no good but referenced at
// https://wiki.mongodb.com/display/10GEN/Multi-hemisphere+%28BigPolygon%29+queries
CRS.properties.name = 'urn:mongodb:crs:strictwinding:EPSG:4326';

shenzhenPoly.crs = CRS;
curs = coll.find({geo: {$geoWithin: {$geometry: shenzhenPoly}}});
assert.throws(function(c){
    c.count();
}, curs, 'expected error with bad CRS');


// this CRS string works for a Big Polygon geo query
CRS.properties.name = 'urn:mongodb:strictwindingcrs:EPSG:4326';
shenzhenPoly.crs = CRS;
curs = coll.find({geo: {$geoWithin: {$geometry: shenzhenPoly}}});
assert.eq(1, curs.count(), 'expected only doc within Big Poly shenzhen triangle');

curs = coll.find({geo: {$geoIntersects: {$geometry: shenzhenPoly}}});
assert.eq(3, curs.count(), 'expected three docs intersecting with Big Poly shenzhen triangle');


// sanity check
// now query for objects outside the Big Poly triangle.  reverse the coordinate traversal direction.
// left foot walking traversal where left foot is inside the polygon
shenzhenPoly.coordinates = [
                             [
                               [ 114.0834046, 22.6648202 ],
                               [ 114.2736054, 22.4047911 ],
                               [ 113.8293457, 22.3819359 ],
                               [ 114.0834046, 22.6648202 ]
                             ]
                           ];

curs = coll.find({geo: {$geoWithin: {$geometry: shenzhenPoly}}});

// sanity check
// all geos should be found except the polygon just inside the northern hemisphere,
// and two MultiPoints within the Shenzhen triangle
// that (rather large) polygon covers the shenzhen triangle
assert.eq(24, curs.count(), 'expected 24 docs outside shenzhen triangle');









// debug
db.big_geodata.count()
db.big_geodata.find({}, {name: 1, 'geo.type': 1}).sort({'geo.type': 1, name: 1})
db.big_geodata.find({geo: {$geoWithin: {$geometry: shenzhenPoly}}}, {name: 1, 'geo.type': 1}).sort({'geo.type': 1, name: 1})


