#include <ecpgtype.h>

struct ECPGtype;
struct ECPGstruct_member
{
	char	   *name;
	struct ECPGtype *typ;
	struct ECPGstruct_member *next;
};

struct ECPGtype
{
	enum ECPGttype typ;
	long		size;			/* For array it is the number of elements.
								 * For varchar it is the maxsize of the
								 * area. */
	union
	{
		struct ECPGtype *element;		/* For an array this is the type
										 * of the element */

		struct ECPGstruct_member *members;
		/* A pointer to a list of members. */
	}			u;
};

/* Everything is malloced. */
void		ECPGmake_struct_member(char *, struct ECPGtype *, struct ECPGstruct_member **);
struct ECPGtype *ECPGmake_simple_type(enum ECPGttype, long);
struct ECPGtype *ECPGmake_varchar_type(enum ECPGttype, long);
struct ECPGtype *ECPGmake_array_type(struct ECPGtype *, long);
struct ECPGtype *ECPGmake_struct_type(struct ECPGstruct_member *);
struct ECPGstruct_member * ECPGstruct_member_dup(struct ECPGstruct_member *);

/* Frees a type. */
void		ECPGfree_struct_member(struct ECPGstruct_member *);
void		ECPGfree_type(struct ECPGtype *);

/* Dump a type.
   The type is dumped as:
   type-tag <comma> reference-to-variable <comma> arrsize <comma> size <comma>
   Where:
   type-tag is one of the simple types or varchar.
   reference-to-variable can be a reference to a struct element.
   arrsize is the size of the array in case of array fetches. Otherwise 0.
   size is the maxsize in case it is a varchar. Otherwise it is the size of
	   the variable (required to do array fetches of structs).
 */
void		ECPGdump_a_type(FILE *, const char *, struct ECPGtype *, const char *, struct ECPGtype *, const char *, const char *);

/* A simple struct to keep a variable and its type. */
struct ECPGtemp_type
{
	struct ECPGtype *typ;
	const char *name;
};

extern const char *ECPGtype_name(enum ECPGttype typ);

/* some stuff for whenever statements */
enum WHEN_TYPE
{
	W_NOTHING,
	W_CONTINUE,
	W_BREAK,
	W_SQLPRINT,
	W_GOTO,
	W_DO,
	W_STOP
};

struct when
{
	enum WHEN_TYPE	code;
	char	   *command;
	char	   *str;
};

struct index
{
	int			index1;
	int			index2;
	char	   *str;
};

struct this_type
{
	enum ECPGttype 	type_enum;
	char	   	*type_str;
	int		type_dimension;
	int		type_index;
};

struct _include_path
{
	char	   *path;
	struct _include_path *next;
};

struct cursor
{
	char	   *name;
	char	   *command;
	char	   *connection;
	struct arguments *argsinsert;
	struct arguments *argsresult;
	struct cursor *next;
};

struct typedefs
{
	char       *name;
	struct this_type  *type;
	struct ECPGstruct_member *struct_member_list;
	struct typedefs *next;
};

struct _defines
{
	char	   *old;
	char	   *new;
	struct _defines *next;
};

/* This is a linked list of the variable names and types. */
struct variable
{
	char	   *name;
	struct ECPGtype *type;
	int			brace_level;
	struct variable *next;
};

struct arguments
{
	struct variable *variable;
	struct variable *indicator;
	struct arguments *next;
};
