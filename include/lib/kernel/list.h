#ifndef __LIB_KERNEL_LIST_H
#define __LIB_KERNEL_LIST_H

/* 이중 연결 리스트 (Doubly linked list).
 *
 * 이 이중 연결 리스트 구현은 동적 할당 메모리를
 * 사용해야 할 필요가 없습니다. 대신, 리스트 요소가 될 가능성이 있는
 * 각 구조체는 `struct list_elem` 멤버를 내장(embed)해야 합니다.
 * 모든 리스트 함수는 이 `struct list_elem`을 기준으로 동작합니다.
 * `list_entry` 매크로는 `struct list_elem`에서
 * 그것을 포함하는 구조체 객체로 다시 변환할 수 있게 해줍니다.
 *
 * 예를 들어, `struct foo`의 리스트가 필요하다고 가정해 봅시다.
 * `struct foo`는 다음과 같이 `struct list_elem` 멤버를
 * 포함해야 합니다:
 *
 * struct foo {
 * struct list_elem elem;
 * int bar;
 * ...other members...
 * };
 *
 * 그러면 `struct foo`의 리스트는 다음과 같이
 * 선언되고 초기화될 수 있습니다:
 *
 * struct list foo_list;
 *
 * list_init (&foo_list);
 *
 * 순회(Iteration)는 `struct list_elem`에서 그것을 감싸는(enclosing)
 * 구조체로 다시 변환해야 하는 일반적인 상황입니다.
 * 다음은 foo_list를 사용한 예시입니다:
 *
 * struct list_elem *e;
 *
 * for (e = list_begin (&foo_list); e != list_end (&foo_list); e = list_next
 * (e)) { struct foo *f = list_entry (e, struct foo, elem);
 *    ...f로 무언가를 수행...
 * }
 *
 * 실제 리스트 사용 예제는 소스 코드 전반에서 찾을 수 있습니다.
 * 예를 들어, threads 디렉터리의 malloc.c, palloc.c, thread.c
 * 모두가 리스트를 사용합니다.
 *
 * 이 리스트의 인터페이스는 C++ STL의 list<> 템플릿에서
 * 영감을 받았습니다. list<>에 익숙하다면, 이것을 사용하기
 * 쉽다고 느낄 것입니다. 하지만, 이 리스트들은 *어떠한* 타입 검사도
 * 하지 않으며, 다른 정확성 검사도 거의 수행할 수 없다는 점을
 * 강조해야 합니다. 만약 당신이 실수한다면, 크게 데일 것입니다 (it will bite
 * you).
 *
 * 리스트 용어집:
 *
 * - "front": 리스트의 첫 번째 요소. 빈 리스트에서는
 * 정의되지 않음. list_front()에 의해 반환됨.
 *
 * - "back": 리스트의 마지막 요소. 빈 리스트에서는
 * 정의되지 않음. list_back()에 의해 반환됨.
 *
 * - "tail": 비유적으로 리스트의 마지막 요소 바로 뒤에 있는
 * 요소. 빈 리스트에서도 잘 정의됨.
 * list_end()에 의해 반환됨. 앞에서 뒤로 순회할 때
 * 끝 감시자(sentinel)로 사용됨.
 *
 * - "beginning": 비어있지 않은 리스트에서는 front. 빈 리스트에서는
 * tail. list_begin()에 의해 반환됨. 앞에서 뒤로 순회할 때
 * 시작점으로 사용됨.
 *
 * - "head": 비유적으로 리스트의 첫 번째 요소 바로 앞에 있는
 * 요소. 빈 리스트에서도 잘 정의됨.
 * list_rend()에 의해 반환됨. 뒤에서 앞으로 순회할 때
 * 끝 감시자(sentinel)로 사용됨.
 *
 * - "reverse beginning": 비어있지 않은 리스트에서는 back. 빈 리스트에서는
 * head. list_rbegin()에 의해 반환됨. 뒤에서 앞으로 순회할 때
 * 시작점으로 사용됨.
 *
 * - "interior element"(내부 요소): head나 tail이 아닌 요소,
 * 즉, 실제 리스트 요소. 빈 리스트에는
 * 내부 요소가 없음.*/

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* 리스트 요소. */
struct list_elem {
    struct list_elem* prev; /* 이전 리스트 요소. */
    struct list_elem* next; /* 다음 리스트 요소. */
};

/* 리스트. */
struct list {
    struct list_elem head; /* 리스트 head. */
    struct list_elem tail; /* 리스트 tail. */
};

/* 리스트 요소 LIST_ELEM을 가리키는 포인터를
   LIST_ELEM이 내장된(embedded inside) 구조체를 가리키는 포인터로
   변환합니다. 바깥 구조체 STRUCT의 이름과 리스트 요소의
   멤버 이름 MEMBER를 제공하세요. 예시는 파일
   상단의 큰 주석을 참조하세요. */
#define list_entry(LIST_ELEM, STRUCT, MEMBER) \
    ((STRUCT*)((uint8_t*)&(LIST_ELEM)->next - offsetof(STRUCT, MEMBER.next)))

void list_init(struct list*);

/* 리스트 순회. */
struct list_elem* list_begin(struct list*);
struct list_elem* list_next(struct list_elem*);
struct list_elem* list_end(struct list*);

struct list_elem* list_rbegin(struct list*);
struct list_elem* list_prev(struct list_elem*);
struct list_elem* list_rend(struct list*);

struct list_elem* list_head(struct list*);
struct list_elem* list_tail(struct list*);

/* 리스트 삽입. */
void list_insert(struct list_elem*, struct list_elem*);
void list_splice(struct list_elem* before,
                 struct list_elem* first,
                 struct list_elem* last);
void list_push_front(struct list*, struct list_elem*);
void list_push_back(struct list*, struct list_elem*);

/* 리스트 제거. */
struct list_elem* list_remove(struct list_elem*);
struct list_elem* list_pop_front(struct list*);
struct list_elem* list_pop_back(struct list*);

/* 리스트 요소 접근. */
struct list_elem* list_front(struct list*);
struct list_elem* list_back(struct list*);

/* 리스트 속성. */
size_t list_size(struct list*);
bool list_empty(struct list*);

/* 기타. */
void list_reverse(struct list*);

/* 보조 데이터 AUX가 주어진 상태에서, 두 리스트 요소 A와 B의
   값을 비교합니다. A가 B보다 작으면 true를 반환하고,
   A가 B보다 크거나 같으면 false를 반환합니다. */
typedef bool list_less_func(const struct list_elem* a,
                            const struct list_elem* b,
                            void* aux);

/* 정렬된 요소들을 가진 리스트에 대한 연산. */
void list_sort(struct list*, list_less_func*, void* aux);
void list_insert_ordered(struct list*,
                         struct list_elem*,
                         list_less_func*,
                         void* aux);
void list_unique(struct list*,
                 struct list* duplicates,
                 list_less_func*,
                 void* aux);

/* 최대값 및 최소값. */
struct list_elem* list_max(struct list*, list_less_func*, void* aux);
struct list_elem* list_min(struct list*, list_less_func*, void* aux);

#endif /* lib/kernel/list.h */