#include "death.h"
#include <assert.h>
#include "cu.h"

/* Two node kinds that link to each other: a ring of people, each pointing
 * at the next person and at a pet, and each pet pointing back at its
 * owner. */
typedef struct Person {
    long id;
    CuLink next;
    CuLink pet;
} Person;

typedef struct Pet {
    char name;
    CuLink owner;
} Pet;

static void test_ring_with_back_links(CuArena *a)
{
    CuPlex px;
    unsigned person;
    unsigned pet;
    CuLink people[3];
    CuLink rex;
    Person p;
    Pet d;
    Person *cur;
    long i;

    cu_plex_init(&px, a);
    person = CU_PLEX_KIND(&px, Person);
    pet = CU_PLEX_KIND(&px, Pet);
    assert(person == 1 && pet == 2);
    for (i = 0; i < 3; i++) {
        p.id = i;
        p.next = cu_link_null();
        p.pet = cu_link_null();
        people[i] = CU_PLEX_ADD(&px, person, &p);
        assert(!cu_link_is_null(people[i]));
    }
    for (i = 0; i < 3; i++)
        CU_PLEX_GET(&px, Person, person, people[i])->next = people[(i + 1) % 3];
    d.name = 'r';
    d.owner = people[0];
    rex = CU_PLEX_ADD(&px, pet, &d);
    CU_PLEX_GET(&px, Person, person, people[0])->pet = rex;

    /* Around the cycle twice. */
    cur = CU_PLEX_GET(&px, Person, person, people[0]);
    for (i = 0; i < 6; i++) {
        assert(cur->id == i % 3);
        cur = CU_PLEX_GET(&px, Person, person, cur->next);
    }
    /* Out to the pet and back. */
    cur = CU_PLEX_GET(&px, Person, person, people[0]);
    assert(CU_PLEX_GET(&px, Pet, pet, cur->pet)->name == 'r');
    assert(cu_link_eq(CU_PLEX_GET(&px, Pet, pet, cur->pet)->owner, people[0]));
    assert(!cu_plex_failed(&px));
}

/* Removing a node needs no unlinking: every link to it goes stale. */
static void test_removed_node_links_go_stale(CuArena *a)
{
    CuPlex px;
    unsigned person;
    CuLink x;
    CuLink y;
    Person p;

    cu_plex_init(&px, a);
    person = CU_PLEX_KIND(&px, Person);
    p.id = 1;
    p.next = cu_link_null();
    p.pet = cu_link_null();
    x = CU_PLEX_ADD(&px, person, &p);
    p.id = 2;
    p.next = x;
    y = CU_PLEX_ADD(&px, person, &p);
    CU_PLEX_GET(&px, Person, person, x)->next = y;
    assert(cu_plex_remove(&px, y));
    assert(CU_PLEX_GET(&px, Person, person, y) == NULL);
    assert(CU_PLEX_GET(&px, Person, person, CU_PLEX_GET(&px, Person, person, x)->next) == NULL);
    assert(!cu_plex_remove(&px, y));
    /* The slot is reused, but the old link stays stale. */
    p.id = 3;
    assert(!cu_link_eq(CU_PLEX_ADD(&px, person, &p), y));
    assert(CU_PLEX_GET(&px, Person, person, y) == NULL);
    assert(CU_PLEX_GET(&px, Person, person, x)->id == 1);
}

static void test_bad_links_get_null(CuArena *a)
{
    CuPlex px;
    unsigned person;
    unsigned pet;
    CuLink rex;
    CuLink forged;
    Pet d;

    cu_plex_init(&px, a);
    person = CU_PLEX_KIND(&px, Person);
    pet = CU_PLEX_KIND(&px, Pet);
    d.name = 'r';
    d.owner = cu_link_null();
    rex = CU_PLEX_ADD(&px, pet, &d);
    /* Same index and generation, but a pet is not a person. */
    assert(CU_PLEX_GET(&px, Person, person, rex) == NULL);
    assert(CU_PLEX_GET(&px, Pet, pet, rex) != NULL);
    assert(CU_PLEX_GET(&px, Pet, pet, cu_link_null()) == NULL);
    assert(!cu_plex_remove(&px, cu_link_null()));
    forged = rex;
    forged.kind = 99;
    assert(CU_PLEX_GET(&px, Pet, pet, forged) == NULL);
    assert(!cu_plex_remove(&px, forged));
    forged = rex;
    forged.h.gen++;
    assert(CU_PLEX_GET(&px, Pet, pet, forged) == NULL);
    assert(CU_PLEX_GET(&px, Pet, pet, rex)->name == 'r');
}

static void test_link_equality(void)
{
    CuLink a;
    CuLink b;

    a.kind = 1;
    a.h.index = 4;
    a.h.gen = 2;
    b = a;
    assert(cu_link_eq(a, b));
    b.kind = 2;
    assert(!cu_link_eq(a, b));
    b = a;
    b.h.gen = 3;
    assert(!cu_link_eq(a, b));
    b = a;
    b.h.index = 5;
    assert(!cu_link_eq(a, b));
    assert(cu_link_is_null(cu_link_null()));
    assert(cu_link_eq(cu_link_null(), cu_link_null()));
    assert(!cu_link_is_null(a));
}

/* One-byte nodes are an ordinary size, not a special case. */
static void test_byte_kind(CuArena *a)
{
    CuPlex px;
    unsigned letter;
    CuLink link;
    char c;

    cu_plex_init(&px, a);
    letter = CU_PLEX_KIND(&px, char);
    assert(letter == 1);
    c = 'q';
    link = CU_PLEX_ADD(&px, letter, &c);
    assert(*CU_PLEX_GET(&px, char, letter, link) == 'q');
}

/* A node size whose pool page overflows size_t makes the add fail without
 * touching malloc; the failure returns before copying. */
static void test_failed_add_is_sticky(CuArena *a)
{
    CuPlex px;
    unsigned big;
    unsigned small;
    size_t huge;
    long v;

    cu_plex_init(&px, a);
    huge = (size_t)-1 >> 1;
    big = cu_plex_kind(&px, huge);
    small = CU_PLEX_KIND(&px, long);
    v = 1;
    assert(cu_link_is_null(cu_plex_add_(&px, big, &v, huge)));
    assert(cu_plex_failed(&px));
    assert(cu_link_is_null(CU_PLEX_ADD(&px, small, &v)));
    assert(cu_plex_kind(&px, sizeof(long)) == 0);
}

/* Run n fails allocation n alone, so each allocation fails in turn while
 * the ones after it succeed; the runs end when one makes no more than n
 * allocations, and only that run may succeed. */
static void test_fails_at_every_allocation(int debug)
{
    CuArena a;
    CuPlex px;
    unsigned person;
    unsigned pet;
    CuLink x;
    CuLink rex;
    Person p;
    Pet d;
    size_t at;
    int injected;
    int done;

    for (at = 0;; at++) {
        cu_arena_init_with(&a, 64, debug);
        cu_arena_fail_at(&a, at);
        cu_plex_init(&px, &a);
        /* A kind id is 0 exactly when registration failed. */
        person = CU_PLEX_KIND(&px, Person);
        assert((person == 0) == cu_plex_failed(&px));
        pet = CU_PLEX_KIND(&px, Pet);
        assert((pet == 0) == cu_plex_failed(&px));
        x = cu_link_null();
        rex = cu_link_null();
        if (pet != 0) {
            p.id = 1;
            p.next = cu_link_null();
            p.pet = cu_link_null();
            x = CU_PLEX_ADD(&px, person, &p);
            d.name = 'r';
            d.owner = x;
            rex = CU_PLEX_ADD(&px, pet, &d);
        }
        done = !cu_link_is_null(rex);
        if (done) {
            assert(CU_PLEX_GET(&px, Pet, pet, rex)->name == 'r');
            assert(CU_PLEX_GET(&px, Person, person, x)->id == 1);
        } else {
            /* Whatever failed marked the plex, and later steps failed too. */
            assert(cu_plex_failed(&px));
            assert(person == 0 || pet == 0 || cu_link_is_null(rex));
            if (person == 0)
                assert(pet == 0);
        }
        /* The run succeeds exactly when the failure was never reached. */
        injected = a.attempts > at;
        assert(done == !injected);
        cu_arena_release(&a);
        if (!injected)
            break;
    }
}

#ifdef CU_DEATH_TESTS
/* Must-use results land here: asserting success inside a death test would
 * let an assert, not the contract check, supply the abort. */
static unsigned kind_sink;
static CuLink link_sink;

static void get_unknown_kind(void)
{
    CuArena a;
    CuPlex px;

    cu_arena_init(&a);
    cu_plex_init(&px, &a);
    kind_sink = CU_PLEX_KIND(&px, Person);
    (void)CU_PLEX_GET(&px, Person, 2, cu_link_null());
}

static void add_unknown_kind(void)
{
    CuArena a;
    CuPlex px;
    Person p;

    cu_arena_init(&a);
    cu_plex_init(&px, &a);
    p.id = 0;
    p.next = cu_link_null();
    p.pet = cu_link_null();
    link_sink = CU_PLEX_ADD(&px, 0, &p);
}

static void get_wrong_size(void)
{
    CuArena a;
    CuPlex px;
    unsigned person;

    cu_arena_init(&a);
    cu_plex_init(&px, &a);
    person = CU_PLEX_KIND(&px, Person);
    (void)CU_PLEX_GET(&px, Pet, person, cu_link_null());
}

static void add_wrong_size(void)
{
    CuArena a;
    CuPlex px;
    unsigned person;
    Pet d;

    cu_arena_init(&a);
    cu_plex_init(&px, &a);
    person = CU_PLEX_KIND(&px, Person);
    d.name = 'x';
    d.owner = cu_link_null();
    link_sink = CU_PLEX_ADD(&px, person, &d);
}

static void kind_zero_size(void)
{
    CuArena a;
    CuPlex px;

    cu_arena_init(&a);
    cu_plex_init(&px, &a);
    kind_sink = cu_plex_kind(&px, 0);
}

static void kind_null_plex(void)
{
    kind_sink = cu_plex_kind(NULL, sizeof(long));
}

static void null_arena(void)
{
    CuPlex px;

    cu_plex_init(&px, NULL);
}

/* Contract violations abort. */
static void death_tests(void)
{
    (void)kind_sink;
    (void)link_sink;
    expect_abort(get_unknown_kind);
    expect_abort(add_unknown_kind);
    expect_abort(get_wrong_size);
    expect_abort(add_wrong_size);
    expect_abort(kind_zero_size);
    expect_abort(null_arena);
    expect_abort(kind_null_plex);
}
#endif

int main(void)
{
    CuArena a;

    cu_arena_init(&a);
    test_ring_with_back_links(&a);
    test_removed_node_links_go_stale(&a);
    test_bad_links_get_null(&a);
    test_link_equality();
    test_byte_kind(&a);
    test_failed_add_is_sticky(&a);
    test_fails_at_every_allocation(0);
    test_fails_at_every_allocation(1);
    cu_arena_release(&a);
#ifdef CU_DEATH_TESTS
    death_tests();
#endif
    return 0;
}
