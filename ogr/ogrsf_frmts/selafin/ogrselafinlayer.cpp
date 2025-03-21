/******************************************************************************
 * Project:  Selafin importer
 * Purpose:  Implementation of OGRSelafinLayer class.
 * Author:   François Hissel, francois.hissel@gmail.com
 *
 ******************************************************************************
 * Copyright (c) 2014,  François Hissel <francois.hissel@gmail.com>
 *
 * SPDX-License-Identifier: MIT
 ****************************************************************************/

#include <cstdlib>
#include "cpl_conv.h"
#include "cpl_string.h"
#include "ogr_p.h"
#include "io_selafin.h"
#include "ogr_selafin.h"
#include "cpl_error.h"
#include "cpl_quad_tree.h"
#include "cpl_vsi_virtual.h"

/************************************************************************/
/*                           Utilities functions                        */
/************************************************************************/
static void MoveOverwrite(VSILFILE *fpDest, VSILFILE *fpSource)
{
    VSIRewindL(fpSource);
    VSIRewindL(fpDest);
    VSIFTruncateL(fpDest, 0);
    char anBuf[0x10000];
    while (!fpSource->Eof() && !fpSource->Error())
    {
        size_t nSize = VSIFReadL(anBuf, 1, sizeof(anBuf), fpSource);
        size_t nLeft = nSize;
        while (nLeft > 0)
            nLeft -= VSIFWriteL(anBuf + nSize - nLeft, 1, nLeft, fpDest);
    }
    VSIFCloseL(fpSource);
    VSIFFlushL(fpDest);
}

/************************************************************************/
/*                            OGRSelafinLayer()                         */
/*       Note that no operation on OGRSelafinLayer is thread-safe       */
/************************************************************************/

OGRSelafinLayer::OGRSelafinLayer(GDALDataset *poDS, const char *pszLayerNameP,
                                 int bUpdateP,
                                 const OGRSpatialReference *poSpatialRefP,
                                 Selafin::Header *poHeaderP, int nStepNumberP,
                                 SelafinTypeDef eTypeP)
    : m_poDS(poDS), eType(eTypeP), bUpdate(CPL_TO_BOOL(bUpdateP)),
      nStepNumber(nStepNumberP), poHeader(poHeaderP),
      poFeatureDefn(
          new OGRFeatureDefn(CPLGetBasenameSafe(pszLayerNameP).c_str())),
      poSpatialRef(nullptr), nCurrentId(-1)
{
#ifdef DEBUG_VERBOSE
    CPLDebug("Selafin", "Opening layer %s", pszLayerNameP);
#endif
    SetDescription(poFeatureDefn->GetName());
    poFeatureDefn->Reference();
    if (eType == POINTS)
        poFeatureDefn->SetGeomType(wkbPoint);
    else
        poFeatureDefn->SetGeomType(wkbPolygon);
    if (poSpatialRefP)
    {
        poSpatialRef = poSpatialRefP->Clone();
        poSpatialRef->SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
    }
    for (int i = 0; i < poHeader->nVar; ++i)
    {
        OGRFieldDefn oFieldDefn(poHeader->papszVariables[i], OFTReal);
        poFeatureDefn->AddFieldDefn(&oFieldDefn);
    }
}

/************************************************************************/
/*                           ~OGRSelafinLayer()                         */
/************************************************************************/
OGRSelafinLayer::~OGRSelafinLayer()
{
#ifdef DEBUG_VERBOSE
    CPLDebug("Selafin", "Closing layer %s", GetName());
#endif
    poFeatureDefn->Release();
    if (poSpatialRef)
        poSpatialRef->Release();
    // poHeader->nRefCount--;
    // if (poHeader->nRefCount==0) delete poHeader;
}

/************************************************************************/
/*                           GetNextFeature()                           */
/************************************************************************/
OGRFeature *OGRSelafinLayer::GetNextFeature()
{
    // CPLDebug("Selafin","GetNextFeature(%li)",nCurrentId+1);
    while (true)
    {
        OGRFeature *poFeature = GetFeature(++nCurrentId);
        if (poFeature == nullptr)
            return nullptr;
        if ((m_poFilterGeom == nullptr ||
             FilterGeometry(poFeature->GetGeometryRef())) &&
            (m_poAttrQuery == nullptr || m_poAttrQuery->Evaluate(poFeature)))
            return poFeature;
        delete poFeature;
    }
}

/************************************************************************/
/*                            ResetReading()                            */
/************************************************************************/
void OGRSelafinLayer::ResetReading()
{
    // CPLDebug("Selafin","ResetReading()");
    nCurrentId = -1;
}

/************************************************************************/
/*                           SetNextByIndex()                           */
/************************************************************************/
OGRErr OGRSelafinLayer::SetNextByIndex(GIntBig nIndex)
{
    // CPLDebug("Selafin","SetNexByIndex(%li)",nIndex);
    if (nIndex < 0 || nIndex >= poHeader->nPoints)
        return OGRERR_FAILURE;
    nCurrentId = nIndex - 1;
    return OGRERR_NONE;
}

/************************************************************************/
/*                           TestCapability()                           */
/************************************************************************/
int OGRSelafinLayer::TestCapability(const char *pszCap)
{
    // CPLDebug("Selafin","TestCapability(%s)",pszCap);
    if (EQUAL(pszCap, OLCRandomRead))
        return TRUE;
    if (EQUAL(pszCap, OLCSequentialWrite))
        return bUpdate;
    if (EQUAL(pszCap, OLCRandomWrite))
        return bUpdate;
    if (EQUAL(pszCap, OLCFastSpatialFilter))
        return FALSE;
    if (EQUAL(pszCap, OLCFastFeatureCount))
        return TRUE;
    if (EQUAL(pszCap, OLCFastGetExtent))
        return TRUE;
    if (EQUAL(pszCap, OLCFastSetNextByIndex))
        return TRUE;
    if (EQUAL(pszCap, OLCCreateField))
        return bUpdate;
    if (EQUAL(pszCap, OLCCreateGeomField))
        return FALSE;
    if (EQUAL(pszCap, OLCDeleteField))
        return bUpdate;
    if (EQUAL(pszCap, OLCReorderFields))
        return bUpdate;
    if (EQUAL(pszCap, OLCAlterFieldDefn))
        return bUpdate;
    if (EQUAL(pszCap, OLCDeleteFeature))
        return bUpdate;
    if (EQUAL(pszCap, OLCStringsAsUTF8))
        return FALSE;
    if (EQUAL(pszCap, OLCTransactions))
        return FALSE;
    if (EQUAL(pszCap, OLCIgnoreFields))
        return FALSE;
    return FALSE;
}

/************************************************************************/
/*                            GetFeature()                              */
/************************************************************************/
OGRFeature *OGRSelafinLayer::GetFeature(GIntBig nFID)
{
    CPLDebug("Selafin", "GetFeature(" CPL_FRMT_GIB ")", nFID);
    if (nFID < 0)
        return nullptr;
    if (eType == POINTS)
    {
        if (nFID >= poHeader->nPoints)
            return nullptr;
        OGRFeature *poFeature = new OGRFeature(poFeatureDefn);
        poFeature->SetGeometryDirectly(new OGRPoint(
            poHeader->paadfCoords[0][nFID], poHeader->paadfCoords[1][nFID]));
        poFeature->SetFID(nFID);
        for (int i = 0; i < poHeader->nVar; ++i)
        {
            VSIFSeekL(poHeader->fp,
                      poHeader->getPosition(nStepNumber, (int)nFID, i),
                      SEEK_SET);
            double nData = 0.0;
            if (Selafin::read_float(poHeader->fp, nData) == 1)
                poFeature->SetField(i, nData);
        }
        return poFeature;
    }
    else
    {
        if (nFID >= poHeader->nElements)
            return nullptr;
        double *anData =
            (double *)VSI_MALLOC2_VERBOSE(sizeof(double), poHeader->nVar);
        if (poHeader->nVar > 0 && anData == nullptr)
            return nullptr;
        for (int i = 0; i < poHeader->nVar; ++i)
            anData[i] = 0;
        OGRFeature *poFeature = new OGRFeature(poFeatureDefn);
        poFeature->SetFID(nFID);
        OGRPolygon *poPolygon = new OGRPolygon();
        OGRLinearRing *poLinearRing = new OGRLinearRing();
        for (int j = 0; j < poHeader->nPointsPerElement; ++j)
        {
            int nPointNum =
                poHeader
                    ->panConnectivity[nFID * poHeader->nPointsPerElement + j] -
                1;
            poLinearRing->addPoint(poHeader->paadfCoords[0][nPointNum],
                                   poHeader->paadfCoords[1][nPointNum]);
            for (int i = 0; i < poHeader->nVar; ++i)
            {
                VSIFSeekL(poHeader->fp,
                          poHeader->getPosition(nStepNumber, nPointNum, i),
                          SEEK_SET);
                double nData = 0.0;
                if (Selafin::read_float(poHeader->fp, nData) == 1)
                    anData[i] += nData;
            }
        }
        poPolygon->addRingDirectly(poLinearRing);
        poPolygon->closeRings();
        poFeature->SetGeometryDirectly(poPolygon);
        if (poHeader->nPointsPerElement)
        {
            for (int i = 0; i < poHeader->nVar; ++i)
                poFeature->SetField(i, anData[i] / poHeader->nPointsPerElement);
        }
        CPLFree(anData);
        return poFeature;
    }
}

/************************************************************************/
/*                           GetFeatureCount()                          */
/************************************************************************/
GIntBig OGRSelafinLayer::GetFeatureCount(int bForce)
{
    // CPLDebug("Selafin","GetFeatureCount(%i)",bForce);
    if (m_poFilterGeom == nullptr && m_poAttrQuery == nullptr)
        return (eType == POINTS) ? poHeader->nPoints : poHeader->nElements;
    if (!bForce)
        return -1;
    int i = 0;
    int nFeatureCount = 0;
    const int nMax = eType == POINTS ? poHeader->nPoints : poHeader->nElements;
    while (i < nMax)
    {
        OGRFeature *poFeature = GetFeature(i++);
        if ((m_poFilterGeom == nullptr ||
             FilterGeometry(poFeature->GetGeometryRef())) &&
            (m_poAttrQuery == nullptr || m_poAttrQuery->Evaluate(poFeature)))
            ++nFeatureCount;
        delete poFeature;
    }
    return nFeatureCount;
}

/************************************************************************/
/*                            IGetExtent()                              */
/************************************************************************/
OGRErr OGRSelafinLayer::IGetExtent(int /* iGeomField*/, OGREnvelope *psExtent,
                                   bool /* bForce*/)
{
    // CPLDebug("Selafin","GetExtent(%i)",bForce);
    if (poHeader->nPoints == 0)
        return OGRERR_NONE;
    CPLRectObj *poObj = poHeader->getBoundingBox();
    psExtent->MinX = poObj->minx;
    psExtent->MaxX = poObj->maxx;
    psExtent->MinY = poObj->miny;
    psExtent->MaxY = poObj->maxy;
    delete poObj;
    return OGRERR_NONE;
}

/************************************************************************/
/*                             ISetFeature()                             */
/************************************************************************/
OGRErr OGRSelafinLayer::ISetFeature(OGRFeature *poFeature)
{
    OGRGeometry *poGeom = poFeature->GetGeometryRef();
    if (poGeom == nullptr)
        return OGRERR_FAILURE;
    if (eType == POINTS)
    {
        // If it is a point layer, it is the "easy" case: we change the
        // coordinates and attributes of the feature and update the file
        if (poGeom->getGeometryType() != wkbPoint)
        {
            CPLError(CE_Failure, CPLE_AppDefined,
                     "The new feature should be of the same Point geometry as "
                     "the existing ones in the layer.");
            return OGRERR_FAILURE;
        }
        OGRPoint *poPoint = poGeom->toPoint();
        GIntBig nFID = poFeature->GetFID();
        poHeader->paadfCoords[0][nFID] = poPoint->getX();
        poHeader->paadfCoords[1][nFID] = poPoint->getY();
        CPLDebug("Selafin", "SetFeature(" CPL_FRMT_GIB ",%f,%f)", nFID,
                 poHeader->paadfCoords[0][nFID],
                 poHeader->paadfCoords[1][nFID]);
        if (VSIFSeekL(
                poHeader->fp,
                88 + 16 + 40 * poHeader->nVar + 48 +
                    ((poHeader->panStartDate != nullptr) ? 32 : 0) + 24 +
                    (poHeader->nElements * poHeader->nPointsPerElement + 2) *
                        4 +
                    (poHeader->nPoints + 2) * 4 + 4 + nFID * 4,
                SEEK_SET) != 0)
            return OGRERR_FAILURE;
        CPLDebug("Selafin", "Write_float(" CPL_FRMT_GUIB ",%f)",
                 VSIFTellL(poHeader->fp),
                 poHeader->paadfCoords[0][nFID] - poHeader->adfOrigin[0]);
        if (Selafin::write_float(poHeader->fp, poHeader->paadfCoords[0][nFID] -
                                                   poHeader->adfOrigin[0]) == 0)
            return OGRERR_FAILURE;
        if (VSIFSeekL(
                poHeader->fp,
                88 + 16 + 40 * poHeader->nVar + 48 +
                    ((poHeader->panStartDate != nullptr) ? 32 : 0) + 24 +
                    (poHeader->nElements * poHeader->nPointsPerElement + 2) *
                        4 +
                    (poHeader->nPoints + 2) * 4 + (poHeader->nPoints + 2) * 4 +
                    4 + nFID * 4,
                SEEK_SET) != 0)
            return OGRERR_FAILURE;
        CPLDebug("Selafin", "Write_float(" CPL_FRMT_GUIB ",%f)",
                 VSIFTellL(poHeader->fp),
                 poHeader->paadfCoords[1][nFID] - poHeader->adfOrigin[1]);
        if (Selafin::write_float(poHeader->fp, poHeader->paadfCoords[1][nFID] -
                                                   poHeader->adfOrigin[1]) == 0)
            return OGRERR_FAILURE;
        for (int i = 0; i < poHeader->nVar; ++i)
        {
            double nData = poFeature->GetFieldAsDouble(i);
            if (VSIFSeekL(poHeader->fp,
                          poHeader->getPosition(nStepNumber, (int)nFID, i),
                          SEEK_SET) != 0)
                return OGRERR_FAILURE;
            if (Selafin::write_float(poHeader->fp, nData) == 0)
                return OGRERR_FAILURE;
        }
    }
    else
    {
        // Else, we have a layer of polygonal elements. Here we consider that
        // the vertices are moved when we change the geometry (which will also
        // lead to a modification in the corresponding point layer). The
        // attributes table can't be changed, because attributes are calculated
        // from those of the vertices. First we check that the new feature is a
        // polygon with the right number of vertices
        if (poGeom->getGeometryType() != wkbPolygon)
        {
            CPLError(CE_Failure, CPLE_AppDefined,
                     "The new feature should be of the same Polygon geometry "
                     "as the existing ones in the layer.");
            return OGRERR_FAILURE;
        }
        OGRLinearRing *poLinearRing = poGeom->toPolygon()->getExteriorRing();
        if (poLinearRing->getNumPoints() != poHeader->nPointsPerElement + 1)
        {
            CPLError(CE_Failure, CPLE_AppDefined,
                     "The new feature should have the same number of vertices "
                     "%d as the existing ones in the layer.",
                     poHeader->nPointsPerElement);
            return OGRERR_FAILURE;
        }
        CPLError(CE_Warning, CPLE_AppDefined,
                 "The attributes of elements layer in Selafin files can't be "
                 "updated.");
        CPLDebug("Selafin", "SetFeature(" CPL_FRMT_GIB ",%f,%f,%f,%f,%f,%f)",
                 poFeature->GetFID(), poLinearRing->getX(0),
                 poLinearRing->getY(0), poLinearRing->getX(1),
                 poLinearRing->getY(1), poLinearRing->getX(2),
                 poLinearRing->getY(
                     2));  //!< This is not safe as we can't be sure there are
                           //!< at least three vertices in the linear ring, but
                           //!< we can assume that for a debug mode
        int nFID = static_cast<int>(poFeature->GetFID());
        // Now we change the coordinates of points in the layer based on the
        // vertices of the new polygon. We don't look at the order of points and
        // we assume that it is the same as in the original layer.
        for (int i = 0; i < poHeader->nPointsPerElement; ++i)
        {
            int nPointId =
                poHeader
                    ->panConnectivity[nFID * poHeader->nPointsPerElement + i] -
                1;
            poHeader->paadfCoords[0][nPointId] = poLinearRing->getX(i);
            poHeader->paadfCoords[1][nPointId] = poLinearRing->getY(i);
            if (VSIFSeekL(
                    poHeader->fp,
                    88 + 16 + 40 * poHeader->nVar + 48 +
                        ((poHeader->panStartDate != nullptr) ? 32 : 0) + 24 +
                        (poHeader->nElements * poHeader->nPointsPerElement +
                         2) *
                            4 +
                        (poHeader->nPoints + 2) * 4 + 4 + nPointId * 4,
                    SEEK_SET) != 0)
                return OGRERR_FAILURE;
            CPLDebug("Selafin", "Write_float(" CPL_FRMT_GUIB ",%f)",
                     VSIFTellL(poHeader->fp),
                     poHeader->paadfCoords[0][nPointId] -
                         poHeader->adfOrigin[0]);
            if (Selafin::write_float(poHeader->fp,
                                     poHeader->paadfCoords[0][nPointId] -
                                         poHeader->adfOrigin[0]) == 0)
                return OGRERR_FAILURE;
            if (VSIFSeekL(
                    poHeader->fp,
                    88 + 16 + 40 * poHeader->nVar + 48 +
                        ((poHeader->panStartDate != nullptr) ? 32 : 0) + 24 +
                        (poHeader->nElements * poHeader->nPointsPerElement +
                         2) *
                            4 +
                        (poHeader->nPoints + 2) * 4 +
                        (poHeader->nPoints + 2) * 4 + 4 + nPointId * 4,
                    SEEK_SET) != 0)
                return OGRERR_FAILURE;
            CPLDebug("Selafin", "Write_float(" CPL_FRMT_GUIB ",%f)",
                     VSIFTellL(poHeader->fp),
                     poHeader->paadfCoords[1][nPointId] -
                         poHeader->adfOrigin[1]);
            if (Selafin::write_float(poHeader->fp,
                                     poHeader->paadfCoords[1][nPointId] -
                                         poHeader->adfOrigin[1]) == 0)
                return OGRERR_FAILURE;
        }
    }
    VSIFFlushL(poHeader->fp);
    poHeader->UpdateFileSize();
    return OGRERR_NONE;
}

/************************************************************************/
/*                           ICreateFeature()                            */
/************************************************************************/
OGRErr OGRSelafinLayer::ICreateFeature(OGRFeature *poFeature)
{
    OGRGeometry *poGeom = poFeature->GetGeometryRef();
    if (poGeom == nullptr)
        return OGRERR_FAILURE;
    if (VSIFSeekL(poHeader->fp, poHeader->getPosition(0), SEEK_SET) != 0)
        return OGRERR_FAILURE;
    if (eType == POINTS)
    {
        // If it is a point layer, it is the "easy" case: we add a new point
        // feature and update the file
        if (poGeom->getGeometryType() != wkbPoint)
        {
            CPLError(CE_Failure, CPLE_AppDefined,
                     "The new feature should be of the same Point geometry as "
                     "the existing ones in the layer.");
            return OGRERR_FAILURE;
        }
        OGRPoint *poPoint = poGeom->toPoint();
        poFeature->SetFID(poHeader->nPoints);
        CPLDebug("Selafin", "CreateFeature(%d,%f,%f)", poHeader->nPoints,
                 poPoint->getX(), poPoint->getY());
        // Change the header to add the new feature
        poHeader->addPoint(poPoint->getX(), poPoint->getY());
    }
    else
    {
        // This is the most difficult case. The user wants to add a polygon
        // element. First we check that it has the same number of vertices as
        // the other polygon elements in the file. If there is no other element,
        // then we define the number of vertices. Every vertex in the layer
        // should have a corresponding point in the corresponding point layer.
        // So if we add a polygon element, we also have to add points in the
        // corresponding layer. The function tries to add as few new points as
        // possible, reusing already existing points. This is generally what the
        // user will expect.

        // First we check that we have the required geometry
        if (poGeom->getGeometryType() != wkbPolygon)
        {
            CPLError(CE_Failure, CPLE_AppDefined,
                     "The new feature should be of the same Polygon geometry "
                     "as the existing ones in the layer.");
            return OGRERR_FAILURE;
        }

        // Now we check that we have the right number of vertices, or if this
        // number was not defined yet (0), we define it at once
        OGRLinearRing *poLinearRing = poGeom->toPolygon()->getExteriorRing();
        poFeature->SetFID(poHeader->nElements);
        CPLDebug("Selafin", "CreateFeature(" CPL_FRMT_GIB ",%f,%f,%f,%f,%f,%f)",
                 poFeature->GetFID(), poLinearRing->getX(0),
                 poLinearRing->getY(0), poLinearRing->getX(1),
                 poLinearRing->getY(1), poLinearRing->getX(2),
                 poLinearRing->getY(
                     2));  //!< This is not safe as we can't be sure there are
                           //!< at least three vertices in the linear ring, but
                           //!< we can assume that for a debug mode
        int nNum = poLinearRing->getNumPoints();
        if (poHeader->nPointsPerElement == 0)
        {
            if (nNum < 4)
            {
                CPLError(CE_Failure, CPLE_AppDefined,
                         "The new feature should have at least 3 vertices.");
                return OGRERR_FAILURE;
            }
            poHeader->nPointsPerElement = nNum - 1;
            if (poHeader->nElements > 0)
            {
                int *panConnectivity =
                    reinterpret_cast<int *>(VSI_REALLOC_VERBOSE(
                        poHeader->panConnectivity,
                        static_cast<size_t>(poHeader->nElements) *
                            poHeader->nPointsPerElement));
                if (panConnectivity == nullptr)
                {
                    VSIFree(poHeader->panConnectivity);
                    poHeader->panConnectivity = nullptr;
                    return OGRERR_FAILURE;
                }
                poHeader->panConnectivity = panConnectivity;
            }
        }
        else
        {
            if (poLinearRing->getNumPoints() != poHeader->nPointsPerElement + 1)
            {
                CPLError(CE_Failure, CPLE_AppDefined,
                         "The new feature should have the same number of "
                         "vertices %d as the existing ones in the layer.",
                         poHeader->nPointsPerElement);
                return OGRERR_FAILURE;
            }
        }

        // Now we look for vertices that are already referenced as points in the
        // file
        int *anMap = (int *)VSI_MALLOC2_VERBOSE(sizeof(int),
                                                poHeader->nPointsPerElement);
        if (anMap == nullptr)
        {
            return OGRERR_FAILURE;
        }
        for (int i = 0; i < poHeader->nPointsPerElement; ++i)
            anMap[i] = -1;
        if (poHeader->nPoints > 0)
        {
            CPLRectObj *poBB = poHeader->getBoundingBox();
            double dfMaxDist =
                (poBB->maxx - poBB->minx) / sqrt((double)(poHeader->nPoints)) /
                1000.0;  //!< Heuristic approach to estimate a maximum distance
                         //!< such that two points are considered equal if they
                         //!< are closer from each other
            dfMaxDist *= dfMaxDist;
            delete poBB;
            for (int i = 0; i < poHeader->nPointsPerElement; ++i)
                anMap[i] = poHeader->getClosestPoint(
                    poLinearRing->getX(i), poLinearRing->getY(i), dfMaxDist);
        }

        // We add new points if needed only
        for (int i = 0; i < poHeader->nPointsPerElement; ++i)
            if (anMap[i] == -1)
            {
                poHeader->addPoint(poLinearRing->getX(i),
                                   poLinearRing->getY(i));
                anMap[i] = poHeader->nPoints - 1;
            }

        // And we update the connectivity table to add the new element
        poHeader->nElements++;
        poHeader->panConnectivity = (int *)CPLRealloc(
            poHeader->panConnectivity,
            sizeof(int) * poHeader->nPointsPerElement * poHeader->nElements);
        for (int i = 0; i < poHeader->nPointsPerElement; ++i)
        {
            poHeader->panConnectivity[poHeader->nPointsPerElement *
                                          (poHeader->nElements - 1) +
                                      i] = anMap[i] + 1;
        }
        poHeader->setUpdated();
        CPLFree(anMap);
    }

    // Now comes the real insertion. Since values have to be inserted nearly
    // everywhere in the file and we don't want to store everything in memory to
    // overwrite it, we create a new copy of it where we write the new values
    const std::string osTempfile = CPLGenerateTempFilenameSafe(nullptr);
    VSILFILE *fpNew = VSIFOpenL(osTempfile.c_str(), "wb+");
    if (fpNew == nullptr)
    {
        CPLError(CE_Failure, CPLE_OpenFailed,
                 "Failed to open temporary file %s with write access, %s.",
                 osTempfile.c_str(), VSIStrerror(errno));
        return OGRERR_FAILURE;
    }
    if (Selafin::write_header(fpNew, poHeader) == 0)
    {
        VSIFCloseL(fpNew);
        VSIUnlink(osTempfile.c_str());
        return OGRERR_FAILURE;
    }
    for (int i = 0; i < poHeader->nSteps; ++i)
    {
        int nLen = 0;
        double dfDate = 0.0;
        if (Selafin::read_integer(poHeader->fp, nLen, true) == 0 ||
            Selafin::read_float(poHeader->fp, dfDate) == 0 ||
            Selafin::read_integer(poHeader->fp, nLen, true) == 0 ||
            Selafin::write_integer(fpNew, 4) == 0 ||
            Selafin::write_float(fpNew, dfDate) == 0 ||
            Selafin::write_integer(fpNew, 4) == 0)
        {
            VSIFCloseL(fpNew);
            VSIUnlink(osTempfile.c_str());
            return OGRERR_FAILURE;
        }
        for (int j = 0; j < poHeader->nVar; ++j)
        {
            double *padfValues = nullptr;
            if (Selafin::read_floatarray(poHeader->fp, &padfValues,
                                         poHeader->nFileSize) == -1)
            {
                VSIFCloseL(fpNew);
                VSIUnlink(osTempfile.c_str());
                return OGRERR_FAILURE;
            }
            padfValues = (double *)CPLRealloc(
                padfValues, sizeof(double) * poHeader->nPoints);
            if (padfValues == nullptr)
            {
                VSIFCloseL(fpNew);
                VSIUnlink(osTempfile.c_str());
                return OGRERR_FAILURE;
            }
            if (eType == POINTS)
                padfValues[poHeader->nPoints - 1] =
                    poFeature->GetFieldAsDouble(j);
            else
                padfValues[poHeader->nPoints - 1] = 0;
            if (Selafin::write_floatarray(fpNew, padfValues,
                                          poHeader->nPoints) == 0)
            {
                CPLFree(padfValues);
                VSIFCloseL(fpNew);
                VSIUnlink(osTempfile.c_str());
                return OGRERR_FAILURE;
            }
            CPLFree(padfValues);
        }
    }

    // If everything went fine, we overwrite the new file with the content of
    // the old one. This way, even if something goes bad, we can still recover
    // the layer. The copy process is format-agnostic.
    MoveOverwrite(poHeader->fp, fpNew);
    VSIUnlink(osTempfile.c_str());
    poHeader->UpdateFileSize();
    return OGRERR_NONE;
}

/************************************************************************/
/*                           CreateField()                              */
/************************************************************************/
OGRErr OGRSelafinLayer::CreateField(const OGRFieldDefn *poField,
                                    CPL_UNUSED int bApproxOK)
{
    CPLDebug("Selafin", "CreateField(%s,%s)", poField->GetNameRef(),
             OGRFieldDefn::GetFieldTypeName(poField->GetType()));
    // Test if the field does not exist yet
    if (poFeatureDefn->GetFieldIndex(poField->GetNameRef()) != -1)
    {
        // Those two lines are copied from CSV driver, but I am not quite sure
        // what they actually do
        if (poFeatureDefn->GetGeomFieldIndex(poField->GetNameRef()) != -1)
            return OGRERR_NONE;
        if (poFeatureDefn->GetGeomFieldIndex(
                CPLSPrintf("geom_%s", poField->GetNameRef())) != -1)
            return OGRERR_NONE;
        CPLError(CE_Failure, CPLE_AppDefined,
                 "Attempt to create field %s, but a field with this name "
                 "already exists.",
                 poField->GetNameRef());
        return OGRERR_FAILURE;
    }
    // Test if the field type is legal (only double precision values are
    // allowed)
    if (poField->GetType() != OFTReal)
    {
        CPLError(
            CE_Failure, CPLE_AppDefined,
            "Attempt to create field of type %s, but this is not supported for "
            "Selafin files (only double precision fields are allowed).",
            poField->GetFieldTypeName(poField->GetType()));
        return OGRERR_FAILURE;
    }
    if (VSIFSeekL(poHeader->fp, poHeader->getPosition(0), SEEK_SET) != 0)
        return OGRERR_FAILURE;
    // Change the header to add the new field
    poHeader->nVar++;
    poHeader->setUpdated();
    poHeader->papszVariables = (char **)CPLRealloc(
        poHeader->papszVariables, sizeof(char *) * poHeader->nVar);
    poHeader->papszVariables[poHeader->nVar - 1] =
        (char *)VSI_MALLOC2_VERBOSE(sizeof(char), 33);
    strncpy(poHeader->papszVariables[poHeader->nVar - 1], poField->GetNameRef(),
            32);
    poHeader->papszVariables[poHeader->nVar - 1][32] = 0;
    poFeatureDefn->AddFieldDefn(poField);

    // Now comes the real insertion. Since values have to be inserted nearly
    // everywhere in the file and we don't want to store everything in memory to
    // overwrite it, we create a new copy of it where we write the new values
    const std::string osTempfile = CPLGenerateTempFilenameSafe(nullptr);
    VSILFILE *fpNew = VSIFOpenL(osTempfile.c_str(), "wb+");
    if (fpNew == nullptr)
    {
        CPLError(CE_Failure, CPLE_OpenFailed,
                 "Failed to open temporary file %s with write access, %s.",
                 osTempfile.c_str(), VSIStrerror(errno));
        return OGRERR_FAILURE;
    }
    if (Selafin::write_header(fpNew, poHeader) == 0)
    {
        VSIFCloseL(fpNew);
        VSIUnlink(osTempfile.c_str());
        return OGRERR_FAILURE;
    }
    for (int i = 0; i < poHeader->nSteps; ++i)
    {
        int nLen = 0;
        double dfDate = 0.0;
        if (Selafin::read_integer(poHeader->fp, nLen, true) == 0 ||
            Selafin::read_float(poHeader->fp, dfDate) == 0 ||
            Selafin::read_integer(poHeader->fp, nLen, true) == 0 ||
            Selafin::write_integer(fpNew, 4) == 0 ||
            Selafin::write_float(fpNew, dfDate) == 0 ||
            Selafin::write_integer(fpNew, 4) == 0)
        {
            VSIFCloseL(fpNew);
            VSIUnlink(osTempfile.c_str());
            return OGRERR_FAILURE;
        }
        double *padfValues = nullptr;
        for (int j = 0; j < poHeader->nVar - 1; ++j)
        {
            if (Selafin::read_floatarray(poHeader->fp, &padfValues,
                                         poHeader->nFileSize) == -1)
            {
                VSIFCloseL(fpNew);
                VSIUnlink(osTempfile.c_str());
                return OGRERR_FAILURE;
            }
            if (Selafin::write_floatarray(fpNew, padfValues,
                                          poHeader->nPoints) == 0)
            {
                CPLFree(padfValues);
                VSIFCloseL(fpNew);
                VSIUnlink(osTempfile.c_str());
                return OGRERR_FAILURE;
            }
            CPLFree(padfValues);
        }
        padfValues =
            (double *)VSI_MALLOC2_VERBOSE(sizeof(double), poHeader->nPoints);
        for (int k = 0; k < poHeader->nPoints; ++k)
            padfValues[k] = 0;
        if (Selafin::write_floatarray(fpNew, padfValues, poHeader->nPoints) ==
            0)
        {
            CPLFree(padfValues);
            VSIFCloseL(fpNew);
            VSIUnlink(osTempfile.c_str());
            return OGRERR_FAILURE;
        }
        CPLFree(padfValues);
    }
    MoveOverwrite(poHeader->fp, fpNew);
    VSIUnlink(osTempfile.c_str());
    poHeader->UpdateFileSize();
    return OGRERR_NONE;
}

/************************************************************************/
/*                           DeleteField()                              */
/************************************************************************/
OGRErr OGRSelafinLayer::DeleteField(int iField)
{
    CPLDebug("Selafin", "DeleteField(%i)", iField);
    if (VSIFSeekL(poHeader->fp, poHeader->getPosition(0), SEEK_SET) != 0)
        return OGRERR_FAILURE;
    // Change the header to remove the field
    poHeader->nVar--;
    poHeader->setUpdated();
    CPLFree(poHeader->papszVariables[iField]);
    for (int i = iField; i < poHeader->nVar; ++i)
        poHeader->papszVariables[i] = poHeader->papszVariables[i + 1];
    poHeader->papszVariables = (char **)CPLRealloc(
        poHeader->papszVariables, sizeof(char *) * poHeader->nVar);
    poFeatureDefn->DeleteFieldDefn(iField);

    // Now comes the real deletion. Since values have to be deleted nearly
    // everywhere in the file and we don't want to store everything in memory to
    // overwrite it, we create a new copy of it where we write the new values
    const std::string osTempfile = CPLGenerateTempFilenameSafe(nullptr);
    VSILFILE *fpNew = VSIFOpenL(osTempfile.c_str(), "wb+");
    if (fpNew == nullptr)
    {
        CPLError(CE_Failure, CPLE_OpenFailed,
                 "Failed to open temporary file %s with write access, %s.",
                 osTempfile.c_str(), VSIStrerror(errno));
        return OGRERR_FAILURE;
    }
    if (Selafin::write_header(fpNew, poHeader) == 0)
    {
        VSIFCloseL(fpNew);
        VSIUnlink(osTempfile.c_str());
        return OGRERR_FAILURE;
    }
    for (int i = 0; i < poHeader->nSteps; ++i)
    {
        int nLen = 0;
        double dfDate = 0.0;
        if (Selafin::read_integer(poHeader->fp, nLen, true) == 0 ||
            Selafin::read_float(poHeader->fp, dfDate) == 0 ||
            Selafin::read_integer(poHeader->fp, nLen, true) == 0 ||
            Selafin::write_integer(fpNew, 4) == 0 ||
            Selafin::write_float(fpNew, dfDate) == 0 ||
            Selafin::write_integer(fpNew, 4) == 0)
        {
            VSIFCloseL(fpNew);
            VSIUnlink(osTempfile.c_str());
            return OGRERR_FAILURE;
        }
        for (int j = 0; j < poHeader->nVar; ++j)
        {
            double *padfValues = nullptr;
            if (Selafin::read_floatarray(poHeader->fp, &padfValues,
                                         poHeader->nFileSize) == -1)
            {
                VSIFCloseL(fpNew);
                VSIUnlink(osTempfile.c_str());
                return OGRERR_FAILURE;
            }
            if (j != iField)
            {
                if (Selafin::write_floatarray(fpNew, padfValues,
                                              poHeader->nPoints) == 0)
                {
                    CPLFree(padfValues);
                    VSIFCloseL(fpNew);
                    VSIUnlink(osTempfile.c_str());
                    return OGRERR_FAILURE;
                }
            }
            CPLFree(padfValues);
        }
    }
    MoveOverwrite(poHeader->fp, fpNew);
    VSIUnlink(osTempfile.c_str());
    poHeader->UpdateFileSize();
    return OGRERR_NONE;
}

/************************************************************************/
/*                          ReorderFields()                             */
/************************************************************************/
OGRErr OGRSelafinLayer::ReorderFields(int *panMap)
{
    CPLDebug("Selafin", "ReorderFields()");
    if (VSIFSeekL(poHeader->fp, poHeader->getPosition(0), SEEK_SET) != 0)
        return OGRERR_FAILURE;
    // Change the header according to the map
    char **papszNew =
        (char **)VSI_MALLOC2_VERBOSE(sizeof(char *), poHeader->nVar);
    for (int i = 0; i < poHeader->nVar; ++i)
        papszNew[i] = poHeader->papszVariables[panMap[i]];
    CPLFree(poHeader->papszVariables);
    poHeader->papszVariables = papszNew;
    poFeatureDefn->ReorderFieldDefns(panMap);

    // Now comes the real change.
    const std::string osTempfile = CPLGenerateTempFilenameSafe(nullptr);
    VSILFILE *fpNew = VSIFOpenL(osTempfile.c_str(), "wb+");
    if (fpNew == nullptr)
    {
        CPLError(CE_Failure, CPLE_OpenFailed,
                 "Failed to open temporary file %s with write access, %s.",
                 osTempfile.c_str(), VSIStrerror(errno));
        return OGRERR_FAILURE;
    }
    if (Selafin::write_header(fpNew, poHeader) == 0)
    {
        VSIFCloseL(fpNew);
        VSIUnlink(osTempfile.c_str());
        return OGRERR_FAILURE;
    }
    double *padfValues = nullptr;
    for (int i = 0; i < poHeader->nSteps; ++i)
    {
        int nLen = 0;
        double dfDate = 0.0;
        if (Selafin::read_integer(poHeader->fp, nLen, true) == 0 ||
            Selafin::read_float(poHeader->fp, dfDate) == 0 ||
            Selafin::read_integer(poHeader->fp, nLen, true) == 0 ||
            Selafin::write_integer(fpNew, 4) == 0 ||
            Selafin::write_float(fpNew, dfDate) == 0 ||
            Selafin::write_integer(fpNew, 4) == 0)
        {
            VSIFCloseL(fpNew);
            VSIUnlink(osTempfile.c_str());
            return OGRERR_FAILURE;
        }
        for (int j = 0; j < poHeader->nVar; ++j)
        {
            if (VSIFSeekL(poHeader->fp, poHeader->getPosition(i, -1, panMap[j]),
                          SEEK_SET) != 0 ||
                Selafin::read_floatarray(poHeader->fp, &padfValues,
                                         poHeader->nFileSize) == -1)
            {
                VSIFCloseL(fpNew);
                VSIUnlink(osTempfile.c_str());
                return OGRERR_FAILURE;
            }
            if (Selafin::write_floatarray(fpNew, padfValues,
                                          poHeader->nPoints) == 0)
            {
                CPLFree(padfValues);
                VSIFCloseL(fpNew);
                VSIUnlink(osTempfile.c_str());
                return OGRERR_FAILURE;
            }
            CPLFree(padfValues);
        }
    }
    MoveOverwrite(poHeader->fp, fpNew);
    VSIUnlink(osTempfile.c_str());
    poHeader->UpdateFileSize();
    return OGRERR_NONE;
}

/************************************************************************/
/*                         AlterFieldDefn()                             */
/************************************************************************/
OGRErr OGRSelafinLayer::AlterFieldDefn(int iField, OGRFieldDefn *poNewFieldDefn,
                                       int /* nFlagsIn */)
{
    CPLDebug("Selafin", "AlterFieldDefn(%i,%s,%s)", iField,
             poNewFieldDefn->GetNameRef(),
             OGRFieldDefn::GetFieldTypeName(poNewFieldDefn->GetType()));
    // Test if the field type is legal (only double precision values are
    // allowed)
    if (poNewFieldDefn->GetType() != OFTReal)
    {
        CPLError(
            CE_Failure, CPLE_AppDefined,
            "Attempt to update field with type %s, but this is not supported "
            "for Selafin files (only double precision fields are allowed).",
            poNewFieldDefn->GetFieldTypeName(poNewFieldDefn->GetType()));
        return OGRERR_FAILURE;
    }
    // Since the field type can't change, only the field name is changed. We
    // change it in the header
    CPLFree(poHeader->papszVariables[iField]);
    poHeader->papszVariables[iField] =
        (char *)VSI_MALLOC2_VERBOSE(sizeof(char), 33);
    strncpy(poHeader->papszVariables[iField], poNewFieldDefn->GetNameRef(), 32);
    poHeader->papszVariables[iField][32] = 0;
    // And we update the file
    if (VSIFSeekL(poHeader->fp, 88 + 16 + 40 * iField, SEEK_SET) != 0)
        return OGRERR_FAILURE;
    if (Selafin::write_string(poHeader->fp, poHeader->papszVariables[iField],
                              32) == 0)
        return OGRERR_FAILURE;
    VSIFFlushL(poHeader->fp);
    poHeader->UpdateFileSize();
    return OGRERR_NONE;
}

/************************************************************************/
/*                          DeleteFeature()                             */
/************************************************************************/
OGRErr OGRSelafinLayer::DeleteFeature(GIntBig nFID)
{
    CPLDebug("Selafin", "DeleteFeature(" CPL_FRMT_GIB ")", nFID);
    if (VSIFSeekL(poHeader->fp, poHeader->getPosition(0), SEEK_SET) != 0)
        return OGRERR_FAILURE;
    // Change the header to delete the feature
    if (eType == POINTS)
        poHeader->removePoint((int)nFID);
    else
    {
        // For elements layer, we only delete the element and not the vertices
        poHeader->nElements--;
        for (int i = (int)nFID; i < poHeader->nElements; ++i)
            for (int j = 0; j < poHeader->nPointsPerElement; ++j)
                poHeader->panConnectivity[poHeader->nPointsPerElement * i + j] =
                    poHeader->panConnectivity[poHeader->nPointsPerElement *
                                                  (i + 1) +
                                              j];
        poHeader->panConnectivity = (int *)CPLRealloc(
            poHeader->panConnectivity,
            sizeof(int) * poHeader->nPointsPerElement * poHeader->nElements);
        poHeader->setUpdated();
    }

    // Now we perform the deletion by creating a new temporary layer
    const std::string osTempfile = CPLGenerateTempFilenameSafe(nullptr);
    VSILFILE *fpNew = VSIFOpenL(osTempfile.c_str(), "wb+");
    if (fpNew == nullptr)
    {
        CPLError(CE_Failure, CPLE_OpenFailed,
                 "Failed to open temporary file %s with write access, %s.",
                 osTempfile.c_str(), VSIStrerror(errno));
        return OGRERR_FAILURE;
    }
    if (Selafin::write_header(fpNew, poHeader) == 0)
    {
        VSIFCloseL(fpNew);
        VSIUnlink(osTempfile.c_str());
        return OGRERR_FAILURE;
    }
    for (int i = 0; i < poHeader->nSteps; ++i)
    {
        int nLen = 0;
        double dfDate = 0.0;
        if (Selafin::read_integer(poHeader->fp, nLen, true) == 0 ||
            Selafin::read_float(poHeader->fp, dfDate) == 0 ||
            Selafin::read_integer(poHeader->fp, nLen, true) == 0 ||
            Selafin::write_integer(fpNew, 4) == 0 ||
            Selafin::write_float(fpNew, dfDate) == 0 ||
            Selafin::write_integer(fpNew, 4) == 0)
        {
            VSIFCloseL(fpNew);
            VSIUnlink(osTempfile.c_str());
            return OGRERR_FAILURE;
        }
        for (int j = 0; j < poHeader->nVar; ++j)
        {
            double *padfValues = nullptr;
            if (Selafin::read_floatarray(poHeader->fp, &padfValues,
                                         poHeader->nFileSize) == -1)
            {
                VSIFCloseL(fpNew);
                VSIUnlink(osTempfile.c_str());
                return OGRERR_FAILURE;
            }
            if (eType == POINTS)
            {
                for (int k = (int)nFID; k <= poHeader->nPoints; ++k)
                    padfValues[k - 1] = padfValues[k];
            }
            if (Selafin::write_floatarray(fpNew, padfValues,
                                          poHeader->nPoints) == 0)
            {
                CPLFree(padfValues);
                VSIFCloseL(fpNew);
                VSIUnlink(osTempfile.c_str());
                return OGRERR_FAILURE;
            }
            CPLFree(padfValues);
        }
    }

    // If everything went fine, we overwrite the new file with the content of
    // the old one. This way, even if something goes bad, we can still recover
    // the layer. The copy process is format-agnostic.
    MoveOverwrite(poHeader->fp, fpNew);
    VSIUnlink(osTempfile.c_str());
    poHeader->UpdateFileSize();

    return OGRERR_NONE;
}
