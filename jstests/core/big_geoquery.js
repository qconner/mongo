
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



var curs = coll.find({geo: {$geoWithin: {$geometry: shenzhenPoly}}});
assert.eq(0, curs.count(), 'expected no docs within shenzhen triangle');


var CRS = {};
CRS.type = 'name';
CRS.properties = {};
// no good but referenced at
// https://wiki.mongodb.com/display/10GEN/Multi-hemisphere+%28BigPolygon%29+queries
CRS.properties.name = 'urn:mongodb:crs:strictwinding:EPSG:4326';

shenzhenPoly.crs = CRS;
curs = coll.find({geo: {$geoWithin: {$geometry: shenzhenPoly}}});

// this form works for a Big Polygon geo query
CRS.properties.name = 'urn:mongodb:strictwindingcrs:EPSG:4326';
shenzhenPoly.crs = CRS;
curs = coll.find({geo: {$geoWithin: {$geometry: shenzhenPoly}}});

assert.eq(0, curs.count(), 'expected no docs within shenzhen triangle');




// now query for objects outside the triangle.  reverse the coordinate traversal direction.
shenzhenPoly.coordinates = [
                             [
                               [ 114.0834046, 22.6648202 ],
                               [ 114.2736054, 22.4047911 ],
                               [ 113.8293457, 22.3819359 ],
                               [ 114.0834046, 22.6648202 ]
                             ]
                           ];

curs = coll.find({geo: {$geoWithin: {$geometry: shenzhenPoly}}});
assert.eq(12, curs.count(), 'expected 12 docs outside shenzhen triangle');

// debug
//db.big_geodata.find({geo: {$geoWithin: {$geometry: shenzhenPoly}}}, {name: 1, 'geo.type': 1}).sort({'geo.type': 1, name: 1})

