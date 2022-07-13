//
//On Windows it might just be <Python.h>.  But on OS it's 
//a framework. The usual way around this is to #include <Python/Python.h>
//on Mac OS.  But I've hacked the -I parameter in CMakeLists.txt instead.
//-James B. 10-Jul-2022.
//
#define PY3K
#include <Python.h>              //yes, even on Mac OS.
#undef   USE_VECTORCLASS_LIBRARY
#define  USE_VECTORCLASS_LIBRARY 1 //Woo!
#define  NO_IMPORT_ARRAY         //Ee-uw. See: https://numpy.org/doc/1.13/reference/c-api.array.html
#include <numpy/arrayobject.h>   //for numpy helper functions
#include <starttree.h>

bool appendStrVector(PyObject* append_me, StrVector& to_me) {
    bool      ok  = false;

    #if (PY_MAJOR_VERSION >= 3)
        PyObject* str = PyObject_Str(append_me);
    #else
        PyObject* str = PyObject_Unicode(append_me);
    #endif

    if (str!=nullptr) {
        Py_ssize_t  len  = 0;
        char const* utf8 = PyUnicode_AsUTF8AndSize(str, &len);
        if (utf8!=nullptr)
        {
            to_me.emplace_back(utf8, len);
        }
        Py_DECREF(str);
    }
    return ok;
}

bool isVectorOfString(const char* vector_name, PyObject*          sequence_arg,
                      StrVector&  sequences,   std::stringstream& complaint) {
    if (sequence_arg==nullptr) {
        complaint << vector_name << " was not supplied.";
        return false;
    }
    PyObject* seq = PySequence_List(sequence_arg);
    if (seq==nullptr) {
        complaint << vector_name << " is not a sequence.";
        return false;
    }
    int number_of_sequences = PySequence_Fast_GET_SIZE(seq);
    for (int i=0; i<number_of_sequences; ++i) {
        PyObject* item = PySequence_Fast_GET_ITEM(seq, i);
        if (item==nullptr) {
            complaint << vector_name << " could not access item " << i << ".";
            return false;
        }
        if (!appendStrVector(item, sequences)) 
        {
            Py_DECREF(seq);
            complaint << vector_name << " could not convert item " << i << " to string.";
            return false;
        }
    }
    Py_DECREF(seq);
    return true;
}

bool appendDoubleVector(PyObject* append_me, DoubleVector& to_me) {
    bool   ok        = false;
    double float_val = PyFloat_AsDouble(append_me);
    if (float_val==-1.0 && PyErr_Occurred())
    {
        return false;
    }
    to_me.emplace_back(float_val);
    return ok;
}

bool isVectorOfDouble(const char*   vector_name, PyObject* vector_arg,
                      DoubleVector& doubles,     const double*& element_data,
                      intptr_t& element_count,   std::stringstream& complaint) {
    element_data  = nullptr;
    element_count = 0;
    if (vector_arg==nullptr) {
        complaint << vector_name << " was not supplied.";
        return false;
    }
    PyObject* seq = PySequence_List(vector_arg);
    if (seq==nullptr) {
        complaint << vector_name << " is not a sequence.";
        return false;
    }
    int number_of_sequences = PySequence_Fast_GET_SIZE(seq);
    for (int i=0; i<number_of_sequences; ++i) {
        PyObject* item = PySequence_Fast_GET_ITEM(seq, i);
        if (item==nullptr) {
            complaint << vector_name << " could not access item " << i << ".";
            return false;
        }
        if (!appendDoubleVector(item, doubles)) 
        {
            Py_DECREF(seq);
            complaint << vector_name << " could not convert item " << i << " to string.";
            return false;
        }
    }
    Py_DECREF(seq);

    element_data  = doubles.data();
    element_count = doubles.size();
    return true;

}

bool isMatrix(PyObject* arg) {
    return PyArray_Check(arg) !=0;
}

bool isMatrixOfDouble(const char*        matrix_name,  PyObject* possible_matrix, 
                      const double*&     element_data, intptr_t& element_count, 
                      std::stringstream& complaint) {            
    element_count = 0;
    if (!isMatrix(possible_matrix)) {
        complaint << matrix_name << " matrix"
                  << " is not a matrix of type Float";
        return false;
    }

    PyArrayObject* matrix = reinterpret_cast<PyArrayObject*>(possible_matrix);
    if (matrix->descr->type_num != NPY_DOUBLE) {
        complaint << matrix_name << " matrix"
                  << " is not a matrix of type Float";
        return false;
    }

    auto dimensions = matrix->nd;
    if (dimensions < 1 || 2<dimensions) {
        complaint << matrix_name << "matrix"
                  << " has " << dimensions << " dimensions"
                  << " (only 1 and 2 dimensional matrices are allowed).";
        return false;                  
    }
    element_count = 1;
    for (int d = 0; d < dimensions; ++d) {
        element_count *= matrix->dimensions[d];
    }
    element_data = reinterpret_cast<double*> PyArray_DATA(matrix);
    return true;
}

bool obeyThreadCount(int number_of_threads, std::stringstream& complaint) {
    #ifdef _OPENMP
        if (0<number_of_threads) {
            int maxThreadCount = omp_get_max_threads();
            if (maxThreadCount < number_of_threads ) {
                //For now, don't care.  Don't even warn about it.
                //Some day, maybe write to complaint and return false
            } else {
                omp_set_num_threads(number_of_threads);
            }
        }
        //If number of threads < 1 use the OMP default.
    #else
        //Ignore it.  Some day, maybe complain.
    #endif
    return true;
}

static PyObject* pydecenttree_constructTree(PyObject* self, PyObject* args, 
                                            PyObject* keywords)
{
    const char* argument_names[] = {
        "algorithm", "sequences", "distances", 
        "number_of_threads", "precision", 
        "verbosity", nullptr
    };
    char*          algorithm_name    = nullptr;
    PyObject*      distance_arg      = nullptr;
    int            number_of_threads = 0;
    int            precision         = 6; //should be default precision
    int            verbosity         = 0; //>0 to say what's happening
    const double*  distance_data     = nullptr;
    intptr_t       distance_entries  = 0;
    PyObject*      sequence_arg      = nullptr;


    if (!PyArg_ParseTupleAndKeywords(args, keywords, "sO!O!iii", 
                                        const_cast<char**>(&argument_names[0]),
                                        &algorithm_name,  &sequence_arg,
                                        &distance_arg, &number_of_threads,
                                        &precision)) 
    {
        return NULL;
    }        
    std::stringstream complaint;
    complaint << "Error: ";
    StartTree::BuilderInterface* algorithm = nullptr;
    if (algorithm_name==nullptr) {
        complaint << "Algorithm name not specified";
    } else {
        algorithm = StartTree::Factory::getInstance().getTreeBuilderByName(algorithm_name);
    }
    std::string  problem;
    std::string  tree_string;
    StrVector    sequences;
    DoubleVector local_distance_vector; //Not needed if distance_matrix is NumPy array
    if (algorithm==nullptr) {
        if (algorithm_name!=nullptr) {
            complaint << "Algorithm " << algorithm_name << " not found.";
        }
    } else {
        if (distance_arg==nullptr) {
            complaint << "No distances were supplied";
        }
        else if (!isVectorOfString("sequencenames", sequence_arg,
                                   sequences, complaint)) {
        }
        else if (sequences.size()<3) {
            complaint << "sequencenames contains only " << sequences.size()
                      << " sequences (must have at least 3).";
        }
        else if (isMatrix(distance_arg) && !isMatrixOfDouble("distance", distance_arg,
                                                 distance_data, distance_entries, complaint)) {
            //Will have set complaint
            //Problem. What if lower precision type is wanted?
        }
        else if (!isVectorOfDouble("distance", distance_arg, local_distance_vector,
                                   distance_data, distance_entries, complaint )) {
            //Will have set complaint
            //To think about later.  Might caller want to request
            //single precision?  Because that uses about half as much
            //memory and is ~ 20% *faster* (if a bit less accurate).
        }
        else if (distance_entries != sequences.size() * sequences.size()) {
            complaint << "There are " << sequences.size() << " sequences"
                      << " but the distance matrix"
                      << " contains " << distance_entries << " elements"
                      << " (should be " << (sequences.size() * sequences.size()) << ").";
        }
        else if (!obeyThreadCount(number_of_threads, complaint)) {
            //Will have set complaint
        }
        else {
            if (verbosity==0) {
                algorithm->beSilent();
            }
            if (!algorithm->constructTreeStringInMemory
                 ( sequences, distance_data, tree_string )) {
                complaint << "Call to constructTreeStringInMemory failed "
                          << " for algorithm " << algorithm_name << ".";
                tree_string.clear();
            }
        }
    }
    if (tree_string.empty()) {
        PyErr_SetString(PyExc_TypeError, complaint.str().c_str());
        return nullptr;
    } else {
        PyObject* tree_result;
        #if PY_MAJOR_VERSION >= 3
            tree_result = PyUnicode_FromString(tree_string.c_str());
        #else
            tree_result = PyString_FromString(tree_string.c_str());
        #endif
        return tree_result;
    }
}

static PyMethodDef pydecenttree_methods[] = {
    { "constructTree", (PyCFunction) pydecenttree_constructTree,
        METH_VARARGS | METH_KEYWORDS, "Construct tree" },
    { NULL, NULL, 0, NULL }
};

static PyModuleDef pydecenttree = {
    PyModuleDef_HEAD_INIT,
    "pydecenttree",
    "", /* module doco */
    -1, /* no per-instance state */
    pydecenttree_methods
};

extern "C" {
    PyMODINIT_FUNC PyInit_pydecenttree(void) {
        return PyModule_Create(&pydecenttree);
    }
};