/**CFile****************************************************************

  FileName    [saigGlaPba.c]

  SystemName  [ABC: Logic synthesis and verification system.]

  PackageName [Sequential AIG package.]

  Synopsis    [Gate level abstraction.]

  Author      [Alan Mishchenko]
  
  Affiliation [UC Berkeley]

  Date        [Ver. 1.0. Started - June 20, 2005.]

  Revision    [$Id: saigGlaPba.c,v 1.00 2005/06/20 00:00:00 alanmi Exp $]

***********************************************************************/

#include "saig.h"
#include "satSolver.h"
#include "satStore.h"

ABC_NAMESPACE_IMPL_START


////////////////////////////////////////////////////////////////////////
///                        DECLARATIONS                              ///
////////////////////////////////////////////////////////////////////////

typedef struct Aig_Gla2Man_t_ Aig_Gla2Man_t;
struct Aig_Gla2Man_t_
{
    // user data
    Aig_Man_t *    pAig;
    int            nConfMax;
    int            nFramesMax;
    int            fVerbose;
    // unrolling
    Vec_Int_t *    vObj2Vec;   // maps obj ID into its vec ID
    Vec_Int_t *    vVec2Var;   // maps vec ID into its sat Var (nFrames per vec ID)
    Vec_Int_t *    vVar2Inf;   // maps sat Var into its frame and obj ID
    Vec_Int_t *    vCla2Obj;   // maps sat Var into its first clause
    // SAT solver
    sat_solver *   pSat;
    // statistics
    int            timePre;
    int            timeSat;
    int            timeTotal;
};

////////////////////////////////////////////////////////////////////////
///                     FUNCTION DEFINITIONS                         ///
////////////////////////////////////////////////////////////////////////

/**Function*************************************************************

  Synopsis    [Adds constant to the solver.]

  Description []
               
  SideEffects []

  SeeAlso     []

***********************************************************************/
static inline int Aig_Gla2AddConst( sat_solver * pSat, int iVar, int fCompl )
{
    lit Lit = toLitCond( iVar, fCompl );
    if ( !sat_solver_addclause( pSat, &Lit, &Lit + 1 ) )
        return 0;
    return 1;
}

/**Function*************************************************************

  Synopsis    [Adds buffer to the solver.]

  Description []
               
  SideEffects []

  SeeAlso     []

***********************************************************************/
static inline int Aig_Gla2AddBuffer( sat_solver * pSat, int iVar0, int iVar1, int fCompl )
{
    lit Lits[2];

    Lits[0] = toLitCond( iVar0, 0 );
    Lits[1] = toLitCond( iVar1, !fCompl );
    if ( !sat_solver_addclause( pSat, Lits, Lits + 2 ) )
        return 0;

    Lits[0] = toLitCond( iVar0, 1 );
    Lits[1] = toLitCond( iVar1, fCompl );
    if ( !sat_solver_addclause( pSat, Lits, Lits + 2 ) )
        return 0;

    return 1;
}

/**Function*************************************************************

  Synopsis    [Adds buffer to the solver.]

  Description []
               
  SideEffects []

  SeeAlso     []

***********************************************************************/
static inline int Aig_Gla2AddNode( sat_solver * pSat, int iVar, int iVar0, int iVar1, int fCompl0, int fCompl1 )
{
    lit Lits[3];

    Lits[0] = toLitCond( iVar, 1 );
    Lits[1] = toLitCond( iVar0, fCompl0 );
    if ( !sat_solver_addclause( pSat, Lits, Lits + 2 ) )
        return 0;

    Lits[0] = toLitCond( iVar, 1 );
    Lits[1] = toLitCond( iVar1, fCompl1 );
    if ( !sat_solver_addclause( pSat, Lits, Lits + 2 ) )
        return 0;

    Lits[0] = toLitCond( iVar, 0 );
    Lits[1] = toLitCond( iVar0, !fCompl0 );
    Lits[2] = toLitCond( iVar1, !fCompl1 );
    if ( !sat_solver_addclause( pSat, Lits, Lits + 3 ) )
        return 0;

    return 1;
}


/**Function*************************************************************

  Synopsis    [Finds existing SAT variable or creates a new one.]

  Description []
               
  SideEffects []

  SeeAlso     []

***********************************************************************/
int Aig_Gla2FetchVar( Aig_Gla2Man_t * p, Aig_Obj_t * pObj, int k )
{
    int i, iVecId, iSatVar;
    assert( k < p->nFramesMax );
    iVecId = Vec_IntEntry( p->vObj2Vec, Aig_ObjId(pObj) );
    if ( iVecId == 0 )
    {
        iVecId = Vec_IntSize( p->vVec2Var ) / p->nFramesMax;
        for ( i = 0; i < p->nFramesMax; i++ )
            Vec_IntPush( p->vVec2Var, 0 );
        Vec_IntWriteEntry( p->vObj2Vec, Aig_ObjId(pObj), iVecId );
    }
    iSatVar = Vec_IntEntry( p->vVec2Var, iVecId * p->nFramesMax + k );
    if ( iSatVar == 0 )
    {
        iSatVar = Vec_IntSize( p->vVar2Inf ) / 2;
        Vec_IntPush( p->vVar2Inf, Aig_ObjId(pObj) );
        Vec_IntPush( p->vVar2Inf, k );
        Vec_IntWriteEntry( p->vVec2Var, iVecId * p->nFramesMax + k, iSatVar );
    }
    return iSatVar;
}

/**Function*************************************************************

  Synopsis    [Assigns variables to the AIG nodes.]

  Description []
               
  SideEffects []

  SeeAlso     []

***********************************************************************/
void Aig_Gla2AssignVars_rec( Aig_Gla2Man_t * p, Aig_Obj_t * pObj, int f )
{
    int nVars = Vec_IntSize(p->vVar2Inf);
    Aig_Gla2FetchVar( p, pObj, f );
    if ( nVars == Vec_IntSize(p->vVar2Inf) )
        return;
    if ( Aig_ObjIsConst1(pObj) )
        return;
    if ( Saig_ObjIsPo( p->pAig, pObj ) )
    {
        Aig_Gla2AssignVars_rec( p, Aig_ObjFanin0(pObj), f );
        return;
    }
    if ( Aig_ObjIsPi( pObj ) )
    {
        if ( Saig_ObjIsLo(p->pAig, pObj) && f > 0 )
            Aig_Gla2AssignVars_rec( p, Aig_ObjFanin0( Saig_ObjLoToLi(p->pAig, pObj) ), f-1 );
        return;
    }
    assert( Aig_ObjIsNode(pObj) );
    Aig_Gla2AssignVars_rec( p, Aig_ObjFanin0(pObj), f );
    Aig_Gla2AssignVars_rec( p, Aig_ObjFanin1(pObj), f );
}

/**Function*************************************************************

  Synopsis    [Creates SAT solver.]

  Description []
               
  SideEffects []

  SeeAlso     []

***********************************************************************/
int Aig_Gla2CreateSatSolver( Aig_Gla2Man_t * p )
{
    Vec_Int_t * vPoLits;
    Aig_Obj_t * pObj;
    int i, f, ObjId, nVars, RetValue = 1;

    // assign variables
//    for ( f = p->nFramesMax - 1; f >= 0; f-- )
    for ( f = 0; f < p->nFramesMax; f++ )
        Aig_Gla2AssignVars_rec( p, Aig_ManPo(p->pAig, 0), f );

    // create SAT solver
    p->pSat = sat_solver_new();
    sat_solver_store_alloc( p->pSat ); 
    sat_solver_setnvars( p->pSat, Vec_IntSize(p->vVar2Inf)/2 );

    // add clauses
    nVars = Vec_IntSize( p->vVar2Inf );
    Vec_IntForEachEntryDouble( p->vVar2Inf, ObjId, f, i )
    {
        if ( ObjId == -1 )
            continue;
        pObj = Aig_ManObj( p->pAig, ObjId );
        if ( Aig_ObjIsNode(pObj) )
        {
            RetValue &= Aig_Gla2AddNode( p->pSat, Aig_Gla2FetchVar(p, pObj, f), 
                                                  Aig_Gla2FetchVar(p, Aig_ObjFanin0(pObj), f), 
                                                  Aig_Gla2FetchVar(p, Aig_ObjFanin1(pObj), f), 
                                                  Aig_ObjFaninC0(pObj), Aig_ObjFaninC1(pObj) );
            Vec_IntPush( p->vCla2Obj, ObjId );
            Vec_IntPush( p->vCla2Obj, ObjId );
            Vec_IntPush( p->vCla2Obj, ObjId );
        }
        else if ( Saig_ObjIsLo(p->pAig, pObj) )
        {
            if ( f == 0 )
            {
                RetValue &= Aig_Gla2AddConst( p->pSat, Aig_Gla2FetchVar(p, pObj, f), 1 );
                Vec_IntPush( p->vCla2Obj, ObjId );
            }
            else
            {
                Aig_Obj_t * pObjLi = Saig_ObjLoToLi(p->pAig, pObj);
                RetValue &= Aig_Gla2AddBuffer( p->pSat, Aig_Gla2FetchVar(p, pObj, f), 
                                                        Aig_Gla2FetchVar(p, Aig_ObjFanin0(pObjLi), f-1), 
                                                        Aig_ObjFaninC0(pObjLi) );
                Vec_IntPush( p->vCla2Obj, ObjId );
                Vec_IntPush( p->vCla2Obj, ObjId );
            }
        }
        else if ( Saig_ObjIsPo(p->pAig, pObj) )
        {
            RetValue &= Aig_Gla2AddBuffer( p->pSat, Aig_Gla2FetchVar(p, pObj, f), 
                                                    Aig_Gla2FetchVar(p, Aig_ObjFanin0(pObj), f), 
                                                    Aig_ObjFaninC0(pObj) );
            Vec_IntPush( p->vCla2Obj, ObjId );
            Vec_IntPush( p->vCla2Obj, ObjId );
        }
        else if ( Aig_ObjIsConst1(pObj) )
        {
            RetValue &= Aig_Gla2AddConst( p->pSat, Aig_Gla2FetchVar(p, pObj, f), 0 );
            Vec_IntPush( p->vCla2Obj, ObjId );
        }
        else assert( Saig_ObjIsPi(p->pAig, pObj) );
    }

    // add output clause
    vPoLits = Vec_IntAlloc( p->nFramesMax );
    for ( f = 0; f < p->nFramesMax; f++ )
        Vec_IntPush( vPoLits, 2 * Aig_Gla2FetchVar(p, Aig_ManPo(p->pAig, 0), f) );
    RetValue &= sat_solver_addclause( p->pSat, Vec_IntArray(vPoLits), Vec_IntArray(vPoLits) + Vec_IntSize(vPoLits) );
    Vec_IntFree( vPoLits );
    Vec_IntPush( p->vCla2Obj, 0 );

    assert( nVars == Vec_IntSize(p->vVar2Inf) );
    assert( ((Sto_Man_t *)p->pSat->pStore)->nClauses == Vec_IntSize(p->vCla2Obj) );

    sat_solver_store_mark_roots( p->pSat ); 
    return RetValue;
}

/**Function*************************************************************

  Synopsis    []

  Description []
               
  SideEffects []

  SeeAlso     []

***********************************************************************/
Aig_Gla2Man_t * Aig_Gla2ManStart( Aig_Man_t * pAig, int nFramesMax )
{
    Aig_Gla2Man_t * p;
    int i;

    p = ABC_CALLOC( Aig_Gla2Man_t, 1 );
    p->pAig       = pAig;

    p->vObj2Vec   = Vec_IntStart( Aig_ManObjNumMax(pAig) );
    p->vVec2Var   = Vec_IntAlloc( 1 << 20 );
    p->vVar2Inf   = Vec_IntAlloc( 1 << 20 );
    p->vCla2Obj   = Vec_IntAlloc( 1 << 20 );

    // skip first vector ID
    p->nFramesMax = nFramesMax;
    for ( i = 0; i < p->nFramesMax; i++ )
        Vec_IntPush( p->vVec2Var, -1 );

    // skip  first SAT variable
    Vec_IntPush( p->vVar2Inf, -1 );
    Vec_IntPush( p->vVar2Inf, -1 );
    return p;
}

/**Function*************************************************************

  Synopsis    []

  Description []
               
  SideEffects []

  SeeAlso     []

***********************************************************************/
void Aig_Gla2ManStop( Aig_Gla2Man_t * p )
{
    Vec_IntFreeP( &p->vObj2Vec );
    Vec_IntFreeP( &p->vVec2Var );
    Vec_IntFreeP( &p->vVar2Inf );
    Vec_IntFreeP( &p->vCla2Obj );

    if ( p->pSat )
        sat_solver_delete( p->pSat );
    ABC_FREE( p );
}


/**Function*************************************************************

  Synopsis    [Finds the set of clauses involved in the UNSAT core.]

  Description []
               
  SideEffects []

  SeeAlso     []

***********************************************************************/
Vec_Int_t * Saig_AbsSolverUnsatCore( sat_solver * pSat, int nConfMax, int fVerbose, int * piRetValue )
{
    Vec_Int_t * vCore;
    void * pSatCnf; 
    Intp_Man_t * pManProof;
    int RetValue, clk = clock();
    if ( piRetValue )
        *piRetValue = -1;
    // solve the problem
    RetValue = sat_solver_solve( pSat, NULL, NULL, (ABC_INT64_T)nConfMax, (ABC_INT64_T)0, (ABC_INT64_T)0, (ABC_INT64_T)0 );
    if ( RetValue == l_Undef )
    {
        printf( "Conflict limit is reached.\n" );
        return NULL;
    }
    if ( RetValue == l_True )
    {
        printf( "The BMC problem is SAT.\n" );
        if ( piRetValue )
            *piRetValue = 0;
        return NULL;
    }
    if ( fVerbose )
    {
        printf( "SAT solver returned UNSAT after %d conflicts.  ", pSat->stats.conflicts );
        ABC_PRT( "Time", clock() - clk );
    }
    assert( RetValue == l_False );
    pSatCnf = sat_solver_store_release( pSat ); 
    // derive the UNSAT core
    clk = clock();
    pManProof = Intp_ManAlloc();
    vCore = (Vec_Int_t *)Intp_ManUnsatCore( pManProof, (Sto_Man_t *)pSatCnf, 0 );
    Intp_ManFree( pManProof );
    Sto_ManFree( (Sto_Man_t *)pSatCnf );
    if ( fVerbose )
    {
        printf( "SAT core contains %d clauses (out of %d).  ", Vec_IntSize(vCore), pSat->stats.clauses );
        ABC_PRT( "Time", clock() - clk );
    }
    return vCore;
}

/**Function*************************************************************

  Synopsis    [Collects abstracted objects.]

  Description []
               
  SideEffects []

  SeeAlso     []

***********************************************************************/
Vec_Int_t * Aig_Gla2ManCollect( Aig_Gla2Man_t * p, Vec_Int_t * vCore )
{
    Vec_Int_t * vResult;
    Aig_Obj_t * pObj;
    int i, ClaId;
    vResult = Vec_IntStart( Aig_ManObjNumMax(p->pAig) );
    Vec_IntWriteEntry( vResult, 0, 1 ); // add const1
    Vec_IntForEachEntry( vCore, ClaId, i )
    {
        pObj = Aig_ManObj( p->pAig, Vec_IntEntry(p->vCla2Obj, ClaId) );
        if ( Saig_ObjIsPi(p->pAig, pObj) || Saig_ObjIsPo(p->pAig, pObj) )
            continue;
        assert( Saig_ObjIsLo(p->pAig, pObj) || Aig_ObjIsNode(pObj) );
        Vec_IntWriteEntry( vResult, Aig_ObjId(pObj), 1 );
    }
    return vResult;
}

/**Function*************************************************************

  Synopsis    [Performs gate-level localization abstraction.]

  Description [Returns array of objects included in the abstraction. This array
  may contain only const1, flop outputs, and internal nodes, that is, objects
  that should have clauses added to the SAT solver.]
               
  SideEffects []

  SeeAlso     []

***********************************************************************/
Vec_Int_t * Aig_Gla2ManTest( Aig_Man_t * pAig, int nFramesMax, int nConfLimit, int TimeLimit, int fVerbose )
{
    int nStart = 0;
    Aig_Gla2Man_t * p;
    Vec_Int_t * vCore, * vResult;
    int clk, clkTotal = clock();
    assert( Saig_ManPoNum(pAig) == 1 );

    if ( fVerbose )
    {
        if ( TimeLimit )
            printf( "Abstracting from frame %d to frame %d with timeout %d sec.\n", nStart, nFramesMax, TimeLimit );
        else
            printf( "Abstracting from frame %d to frame %d with no timeout.\n", nStart, nFramesMax );
    }

    // start the solver
    clk = clock();
    p = Aig_Gla2ManStart( pAig, nFramesMax );
    if ( !Aig_Gla2CreateSatSolver( p ) )
    {
        printf( "Error!  SAT solver became UNSAT.\n" );
        Aig_Gla2ManStop( p );
        return NULL;
    }
    p->timePre += clock() - clk;

    // set runtime limit
    if ( TimeLimit )
        sat_solver_set_runtime_limit( p->pSat, clock() + TimeLimit * CLOCKS_PER_SEC );

    // compute UNSAT core
    clk = clock();
    vCore = Saig_AbsSolverUnsatCore( p->pSat, nConfLimit, fVerbose, NULL );
    if ( vCore == NULL )
    {
        Aig_Gla2ManStop( p );
        return NULL;
    }
    p->timeSat += clock() - clk;
    p->timeTotal = clock() - clkTotal;

    // print stats
    if ( fVerbose )
    {
        ABC_PRTP( "Pre   ", p->timePre,   p->timeTotal );
        ABC_PRTP( "Sat   ", p->timeSat,   p->timeTotal );
        ABC_PRTP( "Total ", p->timeTotal, p->timeTotal );
    }

    // prepare return value
    vResult = Aig_Gla2ManCollect( p, vCore );
    Vec_IntFree( vCore );
    Aig_Gla2ManStop( p );
    return vResult;
}

////////////////////////////////////////////////////////////////////////
///                       END OF FILE                                ///
////////////////////////////////////////////////////////////////////////


ABC_NAMESPACE_IMPL_END
