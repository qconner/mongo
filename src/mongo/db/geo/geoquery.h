/**
*    Copyright (C) 2013 10gen Inc.
*
*    This program is free software: you can redistribute it and/or  modify
*    it under the terms of the GNU Affero General Public License, version 3,
*    as published by the Free Software Foundation.
*
*    This program is distributed in the hope that it will be useful,
*    but WITHOUT ANY WARRANTY; without even the implied warranty of
*    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
*    GNU Affero General Public License for more details.
*
*    You should have received a copy of the GNU Affero General Public License
*    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*
*    As a special exception, the copyright holders give permission to link the
*    code of portions of this program with the OpenSSL library under certain
*    conditions as described in each individual source file and distribute
*    linked combinations including the program with the OpenSSL library. You
*    must comply with the GNU Affero General Public License in all respects for
*    all of the code used other than as permitted herein. If you modify file(s)
*    with this exception, you may extend this exception to your version of the
*    file(s), but you are not obligated to do so. If you do not wish to do so,
*    delete this exception statement from your version. If you delete this
*    exception statement from all source files in the program, then also delete
*    it in the license file.
*/

#pragma once

#include "mongo/base/disallow_copying.h"
#include "mongo/db/geo/geoparser.h"
#include "mongo/db/geo/shapes.h"
#include "mongo/util/mongoutils/str.h"
#include "third_party/s2/s2regionunion.h"

namespace mongo {

    class GeometryContainer {
        MONGO_DISALLOW_COPYING(GeometryContainer);
    public:

        /**
         * Creates an empty geometry container which may then be loaded from BSON or directly.
         */
        GeometryContainer();

        /**
         * Loads an empty GeometryContainer from BSON
         */
        bool parseFrom(const BSONObj &obj);

        /**
         * Is the geometry any of {Point, Line, Polygon}?
         */
        bool isSimpleContainer() const;

        /**
         * Reports the CRS of the contained geometry.
         * TODO: Rework once we have collections of multiple CRSes
         */
        CRS getNativeCRS() const;

        /**
         * Whether or not this geometry can be projected into a particular CRS
         */
        bool supportsProject(CRS crs) const;

        /**
         * Projects the current geometry into the supplied crs.
         * It is an error to call this function if canProjectInto(crs) is false.
         */
        void projectInto(CRS crs);

        /**
         * Minimum distance between this geometry and the supplied point.
         * TODO: Rework and generalize to full GeometryContainer distance
         */
        double minDistance(const PointWithCRS& point) const;

        /**
         * Only polygons (and aggregate types thereof) support contains.
         */
        bool supportsContains() const;

        /**
         * To check containment, we iterate over the otherContainer's geometries.  If we don't
         * contain any sub-geometry of the otherContainer, the otherContainer is not contained
         * within us.  If each sub-geometry of the otherContainer is contained within us, we contain
         * the entire otherContainer.
         */
        bool contains(const GeometryContainer& otherContainer) const;

        /**
         * To check intersection, we iterate over the otherContainer's geometries, checking each
         * geometry to see if we intersect it.  If we intersect one geometry, we intersect the
         * entire other container.
         */
        bool intersects(const GeometryContainer& otherContainer) const;

        // Region which can be used to generate a covering of the query object in the S2 space.
        bool hasS2Region() const;
        const S2Region& getS2Region() const;

        // Region which can be used to generate a covering of the query object in euclidean space.
        bool hasR2Region() const;
        const R2Region& getR2Region() const;

        // Returns a string related to the type of the geometry (for debugging queries)
        std::string getDebugType() const;

        // Needed for 2D wrapping check (for now)
        // TODO: Remove these hacks
        const CapWithCRS* getCapGeometryHack() const;

    private:

        class R2BoxRegion;

        // Does 'this' intersect with the provided type?
        bool intersects(const S2Cell& otherPoint) const;
        bool intersects(const S2Polyline& otherLine) const;
        bool intersects(const S2Polygon& otherPolygon) const;
        // These three just iterate over the geometries and call the 3 methods above.
        bool intersects(const MultiPointWithCRS& otherMultiPoint) const;
        bool intersects(const MultiLineWithCRS& otherMultiLine) const;
        bool intersects(const MultiPolygonWithCRS& otherMultiPolygon) const;

        // Used when 'this' has a polygon somewhere, either in _polygon or _multiPolygon or
        // _geometryCollection.
        bool contains(const S2Cell& otherCell, const S2Point& otherPoint) const;
        bool contains(const S2Polyline& otherLine) const;
        bool contains(const S2Polygon& otherPolygon) const;

        // Only one of these shared_ptrs should be non-NULL.  S2Region is a
        // superclass but it only supports testing against S2Cells.  We need
        // the most specific class we can get.
        scoped_ptr<PointWithCRS> _point;
        scoped_ptr<LineWithCRS> _line;
        scoped_ptr<BoxWithCRS> _box;
        scoped_ptr<PolygonWithCRS> _polygon;
        scoped_ptr<CapWithCRS> _cap;
        scoped_ptr<MultiPointWithCRS> _multiPoint;
        scoped_ptr<MultiLineWithCRS> _multiLine;
        scoped_ptr<MultiPolygonWithCRS> _multiPolygon;
        scoped_ptr<GeometryCollection> _geometryCollection;

        // Cached for use during covering calculations
        // TODO: _s2Region is currently generated immediately - don't necessarily need to do this
        scoped_ptr<S2RegionUnion> _s2Region;
        scoped_ptr<R2Region> _r2Region;
    };

    // TODO: Make a struct, turn parse stuff into something like
    // static Status parseNearQuery(const BSONObj& obj, NearQuery** out);
    class NearQuery {
    public:
        NearQuery()
            : minDistance(0),
              maxDistance(std::numeric_limits<double>::max()),
              isNearSphere(false) { }

        NearQuery(const std::string& f)
            : field(f),
              minDistance(0),
              maxDistance(std::numeric_limits<double>::max()),
              isNearSphere(false) { }

        Status parseFrom(const BSONObj &obj);

        CRS getQueryCRS() const {
            return isNearSphere ? SPHERE : centroid.crs;
        }

        bool unitsAreRadians() const {
            return isNearSphere && FLAT == centroid.crs;
        }

        bool isWrappingQuery() const {
            return SPHERE == centroid.crs && !isNearSphere;
        }

        // The name of the field that contains the geometry.
        std::string field;

        // The starting point of the near search.
        PointWithCRS centroid;

        // Min and max distance from centroid that we're willing to search.
        // Distance is in units of the geometry's CRS, except SPHERE and isNearSphere => radians
        double minDistance;
        double maxDistance;

        // It's either $near or $nearSphere.
        bool isNearSphere;

        std::string toString() const {
            std::stringstream ss;
            ss << " field=" << field;
            ss << " maxdist=" << maxDistance;
            ss << " isNearSphere=" << isNearSphere;
            return ss.str();
        }

    private:
        bool parseLegacyQuery(const BSONObj &obj);
        Status parseNewQuery(const BSONObj &obj);
    };

    // This represents either a $within or a $geoIntersects.
    class GeoQuery {
    public:
        GeoQuery() : field(""), predicate(INVALID) {}
        GeoQuery(const std::string& f) : field(f), predicate(INVALID) {}

        enum Predicate {
            WITHIN,
            INTERSECT,
            INVALID
        };

        bool parseFrom(const BSONObj &obj);

        std::string getField() const { return field; }
        Predicate getPred() const { return predicate; }
        const GeometryContainer& getGeometry() const { return geoContainer; }

    private:
        // Try to parse the provided object into the right place.
        bool parseLegacyQuery(const BSONObj &obj);
        bool parseNewQuery(const BSONObj &obj);

        // Name of the field in the query.
        std::string field;
        GeometryContainer geoContainer;
        Predicate predicate;
    };
}  // namespace mongo
