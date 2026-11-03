// sqlite3ppext.h
//
// The MIT License
//
// Copyright (c) 2015 Wongoo Lee (iwongu at gmail dot com)
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.

#ifndef SQLITE3PPEXT_H
#define SQLITE3PPEXT_H

#include <cstddef>
#include <memory>
#include <new>
#include <exception>
#include <tuple>
#include <type_traits>
#include <utility>

#include "sqlite3pp.h"

namespace sqlite3pp
{
  namespace
  {
    template<size_t N>
    struct Apply {
      template<typename F, typename T, typename... A>
      static inline auto apply(F&& f, T&& t, A&&... a)
        -> decltype(Apply<N-1>::apply(std::forward<F>(f),
                                      std::forward<T>(t),
                                      std::get<N-1>(std::forward<T>(t)),
                                      std::forward<A>(a)...))
      {
        return Apply<N-1>::apply(std::forward<F>(f),
                                 std::forward<T>(t),
                                 std::get<N-1>(std::forward<T>(t)),
                                 std::forward<A>(a)...);
      }
    };

    template<>
    struct Apply<0> {
      template<typename F, typename T, typename... A>
      static inline auto apply(F&& f, T&&, A&&... a)
        -> decltype(std::forward<F>(f)(std::forward<A>(a)...))
      {
        return std::forward<F>(f)(std::forward<A>(a)...);
      }
    };

    template<typename F, typename T>
    inline auto apply(F&& f, T&& t)
      -> decltype(Apply<std::tuple_size<typename std::decay<T>::type>::value>::apply(std::forward<F>(f), std::forward<T>(t)))
    {
      return Apply<std::tuple_size<typename std::decay<T>::type>::value>::apply(
          std::forward<F>(f), std::forward<T>(t));
    }
  }


  namespace ext
  {
    database borrow(sqlite3* pdb);

    class DllExport context : noncopyable
    {
     public:
      explicit context(sqlite3_context* ctx, int nargs = 0, sqlite3_value** values = nullptr);

      int args_count() const;
      int args_bytes(int idx) const;
      int args_type(int idx) const;

      template <class T> T get(int idx) const {
        return get(idx, T());
      }

      // The view is valid only for the duration of this SQLite callback.
      std::string_view get_view(int idx) const;

      void result(int value);
      void result(double value);
      void result(long long int value);
      void result(std::string const& value);
      void result(char const* value, bool fcopy);
      void result(void const* value, int n, bool fcopy);
      void result();
      void result(null_type);
      void result_copy(int idx);
      void result_error(char const* msg);

      void* aggregate_data(int size);
      int aggregate_count();

      template <class... Ts>
      std::tuple<Ts...> to_tuple() {
        return to_tuple_impl(0, *this, std::tuple<Ts...>());
      }

     private:
      int get(int idx, int) const;
      double get(int idx, double) const;
      long long int get(int idx, long long int) const;
      char const* get(int idx, char const*) const;
      std::string get(int idx, std::string) const;
      std::string_view get(int idx, std::string_view) const;
      void const* get(int idx, void const*) const;

      template<class H, class... Ts>
      static inline std::tuple<H, Ts...> to_tuple_impl(int index, const context& c, std::tuple<H, Ts...>&&)
      {
        auto h = std::make_tuple(c.context::get<H>(index));
        return std::tuple_cat(h, to_tuple_impl(++index, c, std::tuple<Ts...>()));
      }
      static inline std::tuple<> to_tuple_impl(int /*index*/, const context& /*c*/, std::tuple<>&&)
      {
        return std::tuple<>();
      }

     private:
      sqlite3_context* ctx_;
      int nargs_;
      sqlite3_value** values_;
    };

    namespace
    {
      template<class T>
      void destroy_user_data(void* data)
      {
        delete static_cast<T*>(data);
      }

      template <class R, class... Ps>
      void functionx_impl(sqlite3_context* ctx, int nargs, sqlite3_value** values)
      {
        try {
          context c(ctx, nargs, values);
          auto f = static_cast<std::function<R (Ps...)>*>(sqlite3_user_data(ctx));
          c.result(apply(*f, c.to_tuple<Ps...>()));
        }
        catch (const std::exception& ex) { sqlite3_result_error(ctx, ex.what(), -1); }
        catch (...) { sqlite3_result_error(ctx, "User-defined function failed", -1); }
      }
    }

    class function : noncopyable
    {
     public:
      using function_handler = std::function<void (context&)>;
      using pfunction_base = std::shared_ptr<void>;

      explicit function(database& db);

      int create(char const* name, function_handler h, int nargs = 0);

      template <class F> int create(char const* name, std::function<F> h) {
        using handler_type = std::function<F>;
        auto* handler = new handler_type(std::move(h));
        return create_function_impl<F>()(db_, handler, name);
      }

     private:

      template<class R, class... Ps>
      struct create_function_impl;

      template<class R, class... Ps>
      struct create_function_impl<R (Ps...)>
      {
        int operator()(sqlite3* db, void* fh, char const* name) {
          using handler_type = std::function<R (Ps...)>;
          return sqlite3_create_function_v2(db, name, sizeof...(Ps), SQLITE_UTF8, fh,
                                            functionx_impl<R, Ps...>, 0, 0,
                                            destroy_user_data<handler_type>);
        }
      };

     private:
      sqlite3* db_;

    };

    namespace
    {
      template<class T>
      struct aggregate_state
      {
        aggregate_state() : initialized(false), failed(false) {}
        bool initialized;
        bool failed;
        typename std::aligned_storage<sizeof(T), alignof(T)>::type storage;
      };

      template <class T, class... Ps>
      void stepx_impl(sqlite3_context* ctx, int nargs, sqlite3_value** values)
      {
        auto* state = static_cast<aggregate_state<T>*>(sqlite3_aggregate_context(ctx, sizeof(aggregate_state<T>)));
        if (!state) { sqlite3_result_error_nomem(ctx); return; }
        if (sqlite3_aggregate_count(ctx) == 1) new (state) aggregate_state<T>();
        if (state->failed) return;
        try {
          if (!state->initialized) {
            new (&state->storage) T();
            state->initialized = true;
          }
          context c(ctx, nargs, values);
          auto* value = reinterpret_cast<T*>(&state->storage);
          apply([&](T* aggregate, auto&&... args) {
            aggregate->step(std::forward<decltype(args)>(args)...);
          }, std::tuple_cat(std::make_tuple(value), c.to_tuple<Ps...>()));
        }
        catch (const std::exception& ex) { state->failed = true; sqlite3_result_error(ctx, ex.what(), -1); }
        catch (...) { state->failed = true; sqlite3_result_error(ctx, "User-defined aggregate failed", -1); }
      }

      template <class T>
      void finishN_impl(sqlite3_context* ctx)
      {
        auto* state = static_cast<aggregate_state<T>*>(sqlite3_aggregate_context(ctx, 0));
        if (!state || !state->initialized) return;
        auto* value = reinterpret_cast<T*>(&state->storage);
        if (state->failed) {
          sqlite3_result_error(ctx, "User-defined aggregate failed", -1);
        } else {
          try {
            context c(ctx);
            c.result(value->finish());
          }
          catch (const std::exception& ex) { sqlite3_result_error(ctx, ex.what(), -1); }
          catch (...) { sqlite3_result_error(ctx, "User-defined aggregate failed", -1); }
        }
        value->~T();
        state->~aggregate_state<T>();
      }
    }

    class DllExport aggregate : noncopyable
    {
     public:
      using function_handler = std::function<void (context&)>;
      using pfunction_base = std::shared_ptr<void>;

      explicit aggregate(database& db);

      int create(char const* name, function_handler s, function_handler f, int nargs = 1);

      template <class T, class... Ps>
      int create(char const* name) {
        return sqlite3_create_function_v2(db_, name, sizeof...(Ps), SQLITE_UTF8, nullptr, nullptr,
                                          stepx_impl<T, Ps...>, finishN_impl<T>, nullptr);
      }

    private:
      sqlite3* db_;

    };

    namespace
    {

        void function_impl(sqlite3_context* ctx, int nargs, sqlite3_value** values)
        {
          try {
            auto f = static_cast<function::function_handler*>(sqlite3_user_data(ctx));
            context c(ctx, nargs, values);
            (*f)(c);
          }
          catch (const std::exception& ex) { sqlite3_result_error(ctx, ex.what(), -1); }
          catch (...) { sqlite3_result_error(ctx, "User-defined function failed", -1); }
        }

        void step_impl(sqlite3_context* ctx, int nargs, sqlite3_value** values)
        {
            try {
              auto p = static_cast<std::pair<aggregate::function_handler, aggregate::function_handler>*>(sqlite3_user_data(ctx));
              context c(ctx, nargs, values);
              p->first(c);
            }
            catch (const std::exception& ex) { sqlite3_result_error(ctx, ex.what(), -1); }
            catch (...) { sqlite3_result_error(ctx, "User-defined aggregate failed", -1); }
        }

        void finalize_impl(sqlite3_context* ctx)
        {
            try {
              auto p = static_cast<std::pair<aggregate::function_handler, aggregate::function_handler>*>(sqlite3_user_data(ctx));
              context c(ctx);
              p->second(c);
            }
            catch (const std::exception& ex) { sqlite3_result_error(ctx, ex.what(), -1); }
            catch (...) { sqlite3_result_error(ctx, "User-defined aggregate failed", -1); }
        }

    } // namespace

    database borrow(sqlite3* pdb) {
        return database(pdb);
    }

    context::context(sqlite3_context* ctx, int nargs, sqlite3_value** values)
        : ctx_(ctx), nargs_(nargs), values_(values)
    {
    }

    int context::args_count() const
    {
        return nargs_;
    }

    int context::args_bytes(int idx) const
    {
        return sqlite3_value_bytes(values_[idx]);
    }

    int context::args_type(int idx) const
    {
        return sqlite3_value_type(values_[idx]);
    }

    int context::get(int idx, int) const
    {
        return sqlite3_value_int(values_[idx]);
    }

    double context::get(int idx, double) const
    {
        return sqlite3_value_double(values_[idx]);
    }

    long long int context::get(int idx, long long int) const
    {
        return sqlite3_value_int64(values_[idx]);
    }

    char const* context::get(int idx, char const*) const
    {
        return reinterpret_cast<char const*>(sqlite3_value_text(values_[idx]));
    }

    std::string context::get(int idx, std::string) const
    {
      return std::string(get(idx, std::string_view{}));
    }

    std::string_view context::get(int idx, std::string_view) const
    {
      const auto* value = sqlite3_value_text(values_[idx]);
      if (!value) return {};
      const auto size = sqlite3_value_bytes(values_[idx]);
      return { reinterpret_cast<const char*>(value), static_cast<size_t>(size) };
    }

    std::string_view context::get_view(int idx) const
    {
      return get(idx, std::string_view{});
    }

    void const* context::get(int idx, void const*) const
    {
        return sqlite3_value_blob(values_[idx]);
    }



    void context::result(int value)
    {
        sqlite3_result_int(ctx_, value);
    }

    void context::result(double value)
    {
        sqlite3_result_double(ctx_, value);
    }

    void context::result(long long int value)
    {
        sqlite3_result_int64(ctx_, value);
    }

    void context::result(std::string const& value)
    {
      result(value.c_str(), true);
    }

    void context::result(char const* value, bool fcopy)
    {
      if (!value) return result();
      sqlite3_result_text(ctx_, value, (int)std::strlen(value), fcopy ? SQLITE_TRANSIENT : SQLITE_STATIC);
    }

    void context::result(void const* value, int n, bool fcopy)
    {
        sqlite3_result_blob(ctx_, value, n, fcopy ? SQLITE_TRANSIENT : SQLITE_STATIC);
    }

    void context::result()
    {
        sqlite3_result_null(ctx_);
    }

    void context::result(null_type)
    {
        sqlite3_result_null(ctx_);
    }

    void context::result_copy(int idx)
    {
        sqlite3_result_value(ctx_, values_[idx]);
    }

    void context::result_error(char const* msg)
    {
        sqlite3_result_error(ctx_, msg, (int)std::strlen(msg));
    }

    void* context::aggregate_data(int size)
    {
        return sqlite3_aggregate_context(ctx_, size);
    }

    int context::aggregate_count()
    {
        return sqlite3_aggregate_count(ctx_);
    }

    function::function(database& db) : db_(db.db_)
    {
    }

    int function::create(char const* name, function_handler h, int nargs)
    {
      auto* handler = new function_handler(std::move(h));
      return sqlite3_create_function_v2(db_, name, nargs, SQLITE_UTF8, handler, function_impl, 0, 0,
                        destroy_user_data<function_handler>);
    }

    aggregate::aggregate(database& db) : db_(db.db_)
    {
    }

    int aggregate::create(char const* name, function_handler s, function_handler f, int nargs)
    {
      using handlers_type = std::pair<function_handler, function_handler>;
      auto* handlers = new handlers_type(std::move(s), std::move(f));
      return sqlite3_create_function_v2(db_, name, nargs, SQLITE_UTF8, handlers, 0, step_impl, finalize_impl,
                        destroy_user_data<handlers_type>);
    }

  } // namespace ext

} // namespace sqlite3pp

#endif